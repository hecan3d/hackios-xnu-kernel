/*
 * Writing into APFS: the file-system tree, one record at a time.
 *
 * What a create or an unlink is, at this level: records in the volume's file-system tree and
 * entries in its object map.  A file needs two records -- an inode and the directory entry
 * that names it -- and its parent's inode record changes with it; an unlink takes both records
 * away and gives the parent back one child.  None of that is visible to a reader until the
 * transaction commits, and the tree machinery in apfs_btree_write.c is what makes the records.
 *
 * What it does not do yet: data.  A file created here is empty; writing bytes into a file,
 * growing one, the extent-reference tree, hard links, xattrs, and rename are still to come.
 * Nor does it merge empty leaves, so repeated creates and unlinks leave the tree sparser than
 * it has to be -- space, not correctness.
 */

#include <stddef.h>
#include <libkern/libkern.h>

#include "apfs_txn_priv.h"

/* a name, its terminating zero, and the key around it */
#define FS_NAME_MAX 1024

/* ------------------------------------------------------------------ little-endian */

static inline uint32_t
f_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static inline uint64_t
f_le64(const uint8_t *p)
{
	return (uint64_t)f_le32(p) | (uint64_t)f_le32(p + 4) << 32;
}

static inline void
f_put16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

static inline void
f_put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

static inline void
f_put64(uint8_t *p, uint64_t v)
{
	f_put32(p, (uint32_t)v);
	f_put32(p + 4, (uint32_t)(v >> 32));
}

/* ------------------------------------------------------------------ keys and their order */

static void
fs_key(uint8_t *out, uint64_t obj_id, uint8_t type)
{
	f_put64(out, (obj_id & APFS_OBJ_ID_MASK) | ((uint64_t)type << APFS_OBJ_TYPE_SHIFT));
}

static uint64_t
fs_key_id(const uint8_t *k)
{
	return f_le64(k) & APFS_OBJ_ID_MASK;
}

static uint8_t
fs_key_type(const uint8_t *k)
{
	return (uint8_t)(f_le64(k) >> APFS_OBJ_TYPE_SHIFT);
}

/*
 * The order of the file-system tree, as mkapfs lays it out and the reader seeks by: object
 * identifier, then kind, then for a directory entry the name's hash (the length bits of that
 * word are not part of the order) and the name itself; for an extended attribute the name; for
 * an extent the offset.
 */
static int
fs_cmp(const uint8_t *a, uint16_t alen, const uint8_t *b, uint16_t blen)
{
	uint64_t ida, idb;
	uint8_t ta, tb;

	if (alen < 8 || blen < 8) {
		return alen < blen ? -1 : alen > blen ? 1 : 0;
	}
	ida = fs_key_id(a);
	idb = fs_key_id(b);
	if (ida != idb) {
		return ida < idb ? -1 : 1;
	}
	ta = fs_key_type(a);
	tb = fs_key_type(b);
	if (ta != tb) {
		return ta < tb ? -1 : 1;
	}
	if (ta == APFS_TYPE_DIR_REC && alen >= 12 && blen >= 12) {
		uint32_t ha = f_le32(a + 8) & ~(uint32_t)0x3ff;
		uint32_t hb = f_le32(b + 8) & ~(uint32_t)0x3ff;
		uint16_t la, lb, n;
		int c;

		if (ha != hb) {
			return ha < hb ? -1 : 1;
		}
		la = (uint16_t)(alen - 12);
		lb = (uint16_t)(blen - 12);
		n = la < lb ? la : lb;
		c = memcmp(a + 12, b + 12, n);
		if (c != 0) {
			return c < 0 ? -1 : 1;
		}
		return la < lb ? -1 : la > lb ? 1 : 0;
	}
	if (ta == APFS_TYPE_XATTR && alen >= 10 && blen >= 10) {
		uint16_t la = (uint16_t)(alen - 10), lb = (uint16_t)(blen - 10);
		uint16_t n = la < lb ? la : lb;
		int c = memcmp(a + 10, b + 10, n);

		if (c != 0) {
			return c < 0 ? -1 : 1;
		}
		return la < lb ? -1 : la > lb ? 1 : 0;
	}
	if (ta == APFS_TYPE_FILE_EXTENT && alen >= 16 && blen >= 16) {
		uint64_t oa = f_le64(a + 8), ob = f_le64(b + 8);

		if (oa != ob) {
			return oa < ob ? -1 : 1;
		}
	}
	return 0;
}

/* An object map's keys are (identifier, transaction): both compared as numbers. */
static int
omap_cmp(const uint8_t *a, uint16_t alen, const uint8_t *b, uint16_t blen)
{
	uint64_t x, y;

	if (alen < 16 || blen < 16) {
		return alen < blen ? -1 : alen > blen ? 1 : 0;
	}
	x = f_le64(a);
	y = f_le64(b);
	if (x != y) {
		return x < y ? -1 : 1;
	}
	x = f_le64(a + 8);
	y = f_le64(b + 8);
	return x < y ? -1 : x > y ? 1 : 0;
}

/* The extent-reference tree is keyed by the first block of a run and the record's kind. */
static int
extref_cmp(const uint8_t *a, uint16_t alen, const uint8_t *b, uint16_t blen)
{
	uint64_t ka, kb;
	uint64_t ia, ib;
	uint8_t ta, tb;

	if (alen < 8 || blen < 8) {
		return alen < blen ? -1 : alen > blen ? 1 : 0;
	}
	ka = f_le64(a);
	kb = f_le64(b);
	ia = ka & APFS_OBJ_ID_MASK;
	ib = kb & APFS_OBJ_ID_MASK;
	if (ia != ib) {
		return ia < ib ? -1 : 1;
	}
	ta = (uint8_t)(ka >> APFS_OBJ_TYPE_SHIFT);
	tb = (uint8_t)(kb >> APFS_OBJ_TYPE_SHIFT);
	return ta < tb ? -1 : ta > tb ? 1 : 0;
}

/* ---------------------------------------------------------------- the trees of a volume */

struct fs_ctx {
	struct apfs_vol  *vol;
	struct apfs_txn  *txn;
	struct btp_tree   fs;           /* the file-system tree */
	struct btp_tree   omap;         /* the volume's object map */
	struct btp_tree   extref;       /* the extent-reference tree, when the volume has one */
	bool              has_extref;
	uint64_t          extref_nodes_old;
	int64_t           data_delta;   /* blocks of file contents this change added or took */
	uint8_t          *sb;           /* the volume superblock, a copy to change */
};

static uint64_t fs_next_obj_id(struct fs_ctx *fc);

static int
fs_resolve(void *ctx, apfs_oid_t oid, uint64_t *addr, apfs_xid_t *key_xid)
{
	struct fs_ctx *fc = ctx;

	return apfs_omap_lookup_key(&fc->vol->omap, oid, apfs_txn_xid(fc->txn), addr,
	    key_xid);
}

static int
fs_open(struct fs_ctx *fc, struct apfs_vol *vol, struct apfs_txn *txn)
{
	int error;

	memset(fc, 0, sizeof(*fc));
	fc->vol = vol;
	fc->txn = txn;
	error = apfs_txn_copy_block(txn, vol->sb_addr, &fc->sb);
	if (error) {
		return error;
	}
	/* the tree's new nodes take their identifiers from the volume's counter */
	error = btp_open(&fc->fs, txn, vol->tree.root, vol->tree.root_oid, false, 0, 0, 0x40,
	    APFS_OBJECT_TYPE_FSTREE, fs_cmp, fs_resolve, fc, fs_next_obj_id(fc));
	if (error) {
		return error;
	}
	error = btp_open(&fc->omap, txn, vol->omap.tree.root, 0, true, 16, 16, 0x10,
	    APFS_OBJECT_TYPE_OMAP, omap_cmp, NULL, NULL, 0);
	if (error) {
		btp_close(&fc->fs);
		return error;
	}
	if (vol->extentref_oid != 0) {
		error = btp_open(&fc->extref, txn, vol->extentref_oid, 0, false, 0, 0, 0x50,
		    APFS_OBJECT_TYPE_BLOCKREFTREE, extref_cmp, NULL, NULL, 0);
		if (error) {
			btp_close(&fc->omap);
			btp_close(&fc->fs);
			return error;
		}
		fc->has_extref = true;
		fc->extref_nodes_old = btp_node_count(&fc->extref);
	}
	return 0;
}

static void
fs_close(struct fs_ctx *fc)
{
	if (fc->sb != NULL) {
		apfs_free(fc->sb, apfs_txn_block_size(fc->txn));
		fc->sb = NULL;
	}
	if (fc->has_extref) {
		btp_close(&fc->extref);
	}
	btp_close(&fc->omap);
	btp_close(&fc->fs);
}

/* Every node the file-system tree made goes into the volume's object map. */
static int
fs_publish(struct fs_ctx *fc)
{
	uint32_t i, n = btp_change_count(&fc->fs);
	int error;

#ifndef KERNEL
	if (getenv("APFS_DEBUG_PUB") != NULL) {
		int k;

		for (k = 0; k < (int)n; k++) {
			apfs_oid_t o, oo;
			apfs_xid_t ox;
			uint64_t a;

			(void)btp_change_at(&fc->fs, (uint32_t)k, &o, &a);
			(void)btp_change_old_at(&fc->fs, (uint32_t)k, &oo, &ox);
			fprintf(stderr, "publish: oid %llu addr %llu old %llu/%llu\n",
			    (unsigned long long)o, (unsigned long long)a,
			    (unsigned long long)oo, (unsigned long long)ox);
		}
	}
#endif
	for (i = 0; i < n; i++) {
		apfs_oid_t oid, old_oid;
		apfs_xid_t old_xid;
		uint64_t addr;
		uint8_t key[16], val[16];

		error = btp_change_at(&fc->fs, i, &oid, &addr);
		if (error) {
			return error;
		}
		error = btp_change_old_at(&fc->fs, i, &old_oid, &old_xid);
		if (error) {
			return error;
		}
		/*
		 * The block published for an oid has to be that oid's own.  A block handed out
		 * twice in one transaction, or a node published under two identifiers, leaves
		 * one identifier pointing at the other node's block: every record of the block
		 * it should have named becomes unreachable, and the creates that follow fail
		 * with EEXIST while lookups of the lost inodes fail with ENOENT.  The block
		 * this transaction last staged at that address is what the object map would
		 * name, so the clash is caught before the record is written.
		 */
		if (oid != 0) {
			uint8_t *blk = NULL;

			error = apfs_txn_read_block(fc->txn, addr, &blk);
			if (error) {
				return error;
			}
			if (blk != NULL) {
				uint64_t h_oid = f_le64(blk + 8);
				uint64_t h_xid = f_le64(blk + 16);
				uint32_t h_type = f_le32(blk + 24) & APFS_OBJECT_TYPE_MASK;

				apfs_free(blk, apfs_txn_block_size(fc->txn));
				if (h_oid != oid || h_xid != apfs_txn_xid(fc->txn) ||
				    (h_type != APFS_OBJECT_TYPE_BTREE &&
				    h_type != APFS_OBJECT_TYPE_BTREE_NODE)) {
					apfs_log("fs: publish: oid %llu at block %llu is oid %llu, xid %llu,"
					    " type %#x\n", (unsigned long long)oid,
					    (unsigned long long)addr, (unsigned long long)h_oid,
					    (unsigned long long)h_xid, h_type);
					return EIO;
				}
			}
		}
		if (oid == 0) {
			/*
			 * A node that is gone: its record leaves the map and nothing takes the
			 * place of it.  This comes from the tree itself, which deletes a child
			 * that has lost its last record instead of writing an empty one.
			 */
			if (old_oid != 0 && old_xid != 0) {
				uint8_t old_key[16];

				f_put64(old_key, old_oid);
				f_put64(old_key + 8, old_xid);
				error = btp_update(&fc->omap, BTP_DELETE, old_key, 16, NULL, 0);
				if (error != 0 && error != ENOENT) {
					return error;
				}
			}
			continue;
		}
		if (old_oid != 0 && old_xid != 0) {
			uint8_t old_key[16];

			f_put64(old_key, old_oid);
			f_put64(old_key + 8, old_xid);
			error = btp_update(&fc->omap, BTP_DELETE, old_key, 16, NULL, 0);
			if (error != 0 && error != ENOENT) {
				return error;
			}
		}
		f_put64(key, oid);
		f_put64(key + 8, apfs_txn_xid(fc->txn));
		f_put32(val, 0);
		f_put32(val + 4, apfs_txn_block_size(fc->txn));
		f_put64(val + 8, addr);
		error = btp_update(&fc->omap, BTP_REPLACE, key, 16, val, 16);
		if (error == ENOENT) {
			error = btp_update(&fc->omap, BTP_INSERT, key, 16, val, 16);
		}
		if (error) {
			return error;
		}
	}
	return 0;
}

/* ---------------------------------------------------------------- the volume superblock */

static uint64_t
fs_next_obj_id(struct fs_ctx *fc)
{
	uint64_t id = f_le64(fc->sb + 0xb0);

	/*
	 * The superblock is written by the commit that made these records; a commit that
	 * failed after its tree blocks went out can leave it behind them.  The volume
	 * counted at open what the trees already hold, and no identifier below that may
	 * be handed out: a live B-tree node owns it.
	 */
	if (fc->vol->id_floor > id) {
		id = fc->vol->id_floor;
	}
	return id;
}

static void
fs_set_next_obj_id(struct fs_ctx *fc, uint64_t id)
{
	f_put64(fc->sb + 0xb0, id);
}

/* -------------------------------------------------------------- the two records of a file */

static uint16_t
inode_val(uint8_t *out, uint64_t obj_id, uint64_t parent, uint16_t mode,
    uint32_t owner, uint32_t group, int32_t nlink_or_children, uint64_t when,
    const char *name, size_t name_len)
{
	uint32_t at = (uint32_t)offsetof(struct apfs_j_inode_val, xfields);
	uint32_t data;

	memset(out, 0, at);
	f_put64(out + offsetof(struct apfs_j_inode_val, parent_id), parent);
	f_put64(out + offsetof(struct apfs_j_inode_val, private_id), obj_id);
	f_put64(out + offsetof(struct apfs_j_inode_val, create_time), when);
	f_put64(out + offsetof(struct apfs_j_inode_val, mod_time), when);
	f_put64(out + offsetof(struct apfs_j_inode_val, change_time), when);
	f_put64(out + offsetof(struct apfs_j_inode_val, access_time), when);
	f_put32(out + offsetof(struct apfs_j_inode_val, nchildren), (uint32_t)nlink_or_children);
	f_put16(out + offsetof(struct apfs_j_inode_val, mode), mode);
	f_put32(out + offsetof(struct apfs_j_inode_val, owner), owner);
	f_put32(out + offsetof(struct apfs_j_inode_val, group), group);

	/*
	 * The extended fields: one, the name.  A file's primary name lives here and not only
	 * in the directory entry that points at it, and apfsck asks for it.
	 */
	if (name != NULL && name_len != 0) {
		uint32_t padded = (uint32_t)((name_len + 8) & ~(size_t)7);   /* with the zero, to 8 */

		memset(out + at, 0, 4);
		f_put16(out + at, 1);                    /* xf_num_exts */
		f_put16(out + at + 2, (uint16_t)padded); /* xf_used_data */
		out[at + 4] = APFS_INO_EXT_TYPE_NAME;
		out[at + 5] = APFS_XF_DO_NOT_COPY;
		f_put16(out + at + 6, (uint16_t)(name_len + 1));
		data = at + 8;
		/* the padding after the name is part of the record and has to be zero */
		memset(out + data, 0, (size_t)padded);
		memcpy(out + data, name, name_len);
		out[data + name_len] = 0;
		at = data + padded;
	}
	return (uint16_t)at;
}

/* A directory entry's key: the parent, the kind, the name's hash and length, the name. */
static uint16_t
drec_key(uint8_t *out, uint64_t parent, const char *name, size_t name_len, bool fold)
{
	uint32_t hash = 0;
	size_t i;

	if (name_len == 0 || name_len >= FS_NAME_MAX) {
		return 0;
	}
	if (!apfs_name_hash(name, name_len, fold, &hash)) {
		return 0;
	}
	fs_key(out, parent, APFS_TYPE_DIR_REC);
	/* the name and its terminating zero; the length counts both */
	f_put32(out + 8, (hash << 10) | (uint32_t)(name_len + 1));
	for (i = 0; i < name_len; i++) {
		out[12 + i] = (uint8_t)name[i];
	}
	out[12 + name_len] = 0;
	return (uint16_t)(12 + name_len + 1);
}

static bool
fs_fold(struct apfs_vol *vol)
{
	return (vol->incompat & APFS_INCOMPAT_CASE_INSENSITIVE) != 0;
}

/*
 * The parent's record, changed where it lies: its copy is read whole and put back with one
 * count and the change time different, because everything else in it -- the name, the data
 * stream, the extended fields this does not understand -- has to stay as it was.
 */
static int
fs_write_parent(struct fs_ctx *fc, uint64_t parent, int32_t children, uint64_t when)
{
	uint8_t key[8], *val;
	uint16_t vlen;
	int error;

	val = apfs_alloc(apfs_txn_block_size(fc->txn));
	if (val == NULL) {
		return ENOMEM;
	}
	error = apfs_inode_value_get(fc->vol, parent, val, apfs_txn_block_size(fc->txn), &vlen);
	if (error) {
		apfs_free(val, apfs_txn_block_size(fc->txn));
		return error;
	}
	if (vlen < (uint16_t)offsetof(struct apfs_j_inode_val, xfields)) {
		apfs_free(val, apfs_txn_block_size(fc->txn));
		return EIO;
	}
	f_put32(val + offsetof(struct apfs_j_inode_val, nchildren), (uint32_t)children);
	f_put64(val + offsetof(struct apfs_j_inode_val, change_time), when);
	fs_key(key, parent, APFS_TYPE_INODE);
	error = btp_update(&fc->fs, BTP_REPLACE, key, 8, val, vlen);
	if (error) {
		apfs_log("fs: parent replace: %d\n", error);
	}
	apfs_free(val, apfs_txn_block_size(fc->txn));
	return error;
}

static uint8_t fs_dtype(uint16_t mode);

static int
fs_add_file(struct fs_ctx *fc, uint64_t parent, const char *name, uint16_t mode,
    uint32_t owner, uint32_t group, bool dir, uint64_t when, uint64_t *new_id)
{
	struct apfs_inode pino;
	/* the inode's value: the fixed part, the name's extended field and the name, padded to 8 */
	uint8_t key[FS_NAME_MAX + 16], val[92 + 8 + ((FS_NAME_MAX + 7) & ~(size_t)7)];
	apfs_oid_t id;
	uint16_t klen, vlen;
	int error;
	unsigned attempt;

	error = apfs_inode_get(fc->vol, parent, &pino);
	if (error) {
		apfs_log("fs: parent %llu cannot be read: %d\n",
		    (unsigned long long)parent, error);
		return error;
	}
	if ((pino.mode & APFS_S_IFMT) != APFS_S_IFDIR) {
		return ENOTDIR;
	}
	/*
	 * An identifier already in the tree is not the end of the operation.  The
	 * superblock can be behind the tree (see fs_next_obj_id), and a record left in
	 * the slot by an earlier life of the volume is possible too.  btp_update leaves
	 * the tree as it was when it refuses an insert, so taking the next one costs a
	 * comparison and nothing else; a run of taken slots is skipped in one go.
	 */
	for (attempt = 0; ; attempt++) {
		id = fs_next_obj_id(fc);
		fs_set_next_obj_id(fc, id + 1);
		if (btp_next_oid(&fc->fs) < id + 1) {
			btp_set_next_oid(&fc->fs, id + 1);
		}

		fs_key(key, id, APFS_TYPE_INODE);
		vlen = inode_val(val, id, parent, mode, owner, group, dir ? 0 : 1, when,
		    name, strlen(name));
		/* no resource fork, which is what an empty file is; apfsck insists the flags say so */
		f_put64(val + offsetof(struct apfs_j_inode_val, internal_flags),
		    APFS_INODE_NO_RSRC_FORK);
		error = btp_update(&fc->fs, BTP_INSERT, key, 8, val, vlen);
		if (error != EEXIST) {
			break;
		}
		if (attempt >= 4096) {
			apfs_log("fs: inode insert: %d (no free identifier in %u tries)\n",
			    error, attempt + 1);
			return error;
		}
		if ((attempt & 0xff) == 0) {
			apfs_log("fs: identifier %llu is taken; taking the next\n",
			    (unsigned long long)id);
		}
	}
	if (error) {
		apfs_log("fs: inode insert: %d\n", error);
		return error;
	}
	klen = drec_key(key, parent, name, strlen(name), fs_fold(fc->vol));
	if (klen == 0) {
		return EINVAL;
	}
	f_put64(val, id);
	f_put64(val + 8, when);
	f_put16(val + 16, fs_dtype(mode));
	error = btp_update(&fc->fs, BTP_INSERT, key, klen, val, 18);
	if (error) {
		apfs_log("fs: drec insert: %d\n", error);
		return error;
	}
	error = fs_write_parent(fc, parent, pino.nchildren + 1, when);
	if (error) {
		apfs_log("fs: write parent: %d\n", error);
		return error;
	}
	fs_set_next_obj_id(fc, id + 1);
	/* apfs_num_files at 0xb8, directories at 0xc0, symlinks at 0xc8 */
	{
		uint32_t at = dir ? 0xc0 :
		    ((mode & APFS_S_IFMT) == APFS_S_IFLNK ? 0xc8 : 0xb8);

		f_put64(fc->sb + at, f_le64(fc->sb + at) + 1);
	}
	*new_id = id;
	return 0;
}

static uint16_t xattr_key(uint8_t *out, uint64_t obj_id, const char *name, size_t name_len);

/* The attributes of a file that is going away: each record out of the file-system tree. */
struct xattr_del_ctx {
	struct fs_ctx *fc;
	uint64_t       id;
	int            error;
};

static int
xattr_del_cb(void *arg, const char *name, size_t name_len, const struct apfs_xattr *xa)
{
	struct xattr_del_ctx *c = arg;
	uint8_t key[FS_NAME_MAX + 16];
	uint16_t klen;

	(void)xa;
	klen = xattr_key(key, c->id, name, name_len);
	if (klen == 0) {
		c->error = EINVAL;
		return 1;
	}
	c->error = btp_update(&c->fc->fs, BTP_DELETE, key, klen, NULL, 0);
	if (c->error != 0 && c->error != ENOENT) {
		return 1;
	}
	c->error = 0;
	return 0;
}


/* The name a file should report while its first link is being removed: another entry of the
 * same parent that points at it.  (Links in other directories are not looked for; the inode's
 * own name then stays as it was, which apfsck may call wrong, but nothing reads by it.) */
static int inode_set_name(uint8_t *val, uint16_t *vlen, const char *name, size_t n);

struct other_name {
	uint64_t id;
	const char *skip;
	char name[FS_NAME_MAX];
};

static int
other_name_cb(void *arg, const char *name, size_t name_len, uint64_t file_id, uint8_t dtype)
{
	struct other_name *o = arg;

	(void)dtype;
	if (file_id == o->id && strcmp(name, o->skip) != 0 && name_len < sizeof(o->name)) {
		memcpy(o->name, name, name_len);
		o->name[name_len] = 0;
		return 1;
	}
	return 0;
}

static int
count_cb(void *arg, const char *name, size_t name_len, uint64_t file_id, uint8_t dtype)
{
	unsigned *count = arg;

	(void)name;
	(void)name_len;
	(void)file_id;
	(void)dtype;
	(*count)++;
	return 1;                       /* (the first real entry is enough to stop) */
}

/*
 * The names that are still inside a directory that refused to go.  The container manager asks
 * for a remove of a directory it believes empty; the log says what the tree actually holds
 * there, so the next run knows whether the driver miscounts or the caller never removed them.
 */
struct list_ctx {
	unsigned shown;
	unsigned total;
};

static int
list_cb(void *arg, const char *name, size_t name_len, uint64_t file_id, uint8_t dtype)
{
	struct list_ctx *lc = arg;

	lc->total++;
	if (lc->shown < 12) {
		apfs_log("fs:   inside: %.*s (%llu, type %u)\n", (int)name_len, name,
		    (unsigned long long)file_id, (unsigned)dtype);
		lc->shown++;
	}
	return 1;
}

/*
 * A file's end: its blocks, its data stream, its attributes and its inode record.  The
 * directory entry that named it is not here -- it goes in the transaction that took the
 * name away.  `count_parent` says whether the name's directory still counts it as a child:
 * after an unlink of the last name it does not, because that one was counted then.
 */
static int
fs_delete_inode(struct fs_ctx *fc, uint64_t id, struct apfs_inode *ino, bool dir,
    uint64_t parent, bool count_parent, uint64_t when)
{
	uint8_t key[8];
	int error;

	/*
	 * A file's blocks go back before its records do: each extent's blocks to the space
	 * manager, each extent's entry out of the reference tree, each extent record out of
	 * the file-system tree.
	 */
	if (!dir && ino->private_id != 0) {
		uint64_t off = 0;

		for (;;) {
			struct apfs_extent ex;
			uint8_t ekey[16];

			error = apfs_extent_next(fc->vol, ino->private_id, off, &ex);
			if (error) {
				break;          /* past the last extent: ENOENT */
			}
			if (ex.phys != 0) {
				uint32_t nb = (uint32_t)(ex.length / apfs_txn_block_size(fc->txn));
				uint32_t j;

				for (j = 0; j < nb; j++) {
					error = apfs_txn_free_block_who(fc->txn, ex.phys + j,
					    APFS_FREE_INODE, id);
					if (error) {
						return error;
					}
				}
				fc->data_delta -= (int64_t)nb;
				if (fc->has_extref) {
					uint8_t rkey[8];

					fs_key(rkey, ex.phys, APFS_TYPE_EXTENT);
					error = btp_update(&fc->extref, BTP_DELETE, rkey, 8, NULL, 0);
					if (error != 0 && error != ENOENT) {
						return error;
					}
				}
			}
			fs_key(ekey, ino->private_id, APFS_TYPE_FILE_EXTENT);
			f_put64(ekey + 8, ex.logical);
			error = btp_update(&fc->fs, BTP_DELETE, ekey, 16, NULL, 0);
			if (error != 0 && error != ENOENT) {
				return error;
			}
			if (ex.length == 0) {
				break;
			}
			off = ex.logical + ex.length;
		}
		/* the record that says the stream has extents (its own key: the entry's is still
		 * to be deleted below) */
		{
			uint8_t dkey[8];

			fs_key(dkey, ino->private_id, APFS_TYPE_DSTREAM_ID);
			(void)btp_update(&fc->fs, BTP_DELETE, dkey, 8, NULL, 0);
		}
	}

	/*
	 * The attributes go with the inode: a record left behind is one apfsck knows as an
	 * "xattr record with no inode".  Listed through the library, which reads what the
	 * tree held before this transaction -- attributes are not changed by an unlink.
	 */
	{
		struct xattr_del_ctx xc = { fc, id, 0 };

		(void)apfs_xattr_list(fc->vol, id, xattr_del_cb, &xc);
		if (xc.error) {
			return xc.error;
		}
	}
	fs_key(key, id, APFS_TYPE_INODE);
	error = btp_update(&fc->fs, BTP_DELETE, key, 8, NULL, 0);
	if (error) {
		return error;
	}
	if (count_parent) {
		struct apfs_inode pino;

		error = apfs_inode_get(fc->vol, parent, &pino);
		if (error) {
			return error;
		}
		error = fs_write_parent(fc, parent, pino.nchildren - 1, when);
		if (error) {
			return error;
		}
	}
	{
		uint32_t at = dir ? 0xc0 :
		    ((ino->mode & APFS_S_IFMT) == APFS_S_IFLNK ? 0xc8 : 0xb8);

		f_put64(fc->sb + at, f_le64(fc->sb + at) - 1);
	}
	return 0;
}

/* (does the name have this in it: the driver's own, as strstr is not declared here) */
/* who asked: the process name, so a rename of a container is not an anonymous line */
static const char *
who_name(void)
{
	static char name[32];

	name[0] = 0;
	proc_selfname(name, sizeof(name));
	return name[0] != 0 ? name : "-";
}

static bool
name_has(const char *name, const char *part)
{
	size_t n = strlen(part);

	for (; *name != 0; name++) {
		if (strncmp(name, part, n) == 0) {
			return true;
		}
	}
	return false;
}

static int
fs_remove_file(struct fs_ctx *fc, uint64_t parent, const char *name, bool dir, bool keep,
    uint64_t when)
{
	uint8_t key[FS_NAME_MAX + 16];
	struct apfs_inode ino, pino;
	uint64_t id = 0;
	uint8_t dt = 0;
	uint16_t klen;
	int error;

	klen = drec_key(key, parent, name, strlen(name), fs_fold(fc->vol));
	if (klen == 0) {
		return EINVAL;
	}
	if (dir || name_has(name, "systemgroup") || name_has(name, "lsd")) {
		apfs_log("fs: remove %s %s in %llu by %s\n", dir ? "directory" : "file", name,
		    (unsigned long long)parent, who_name());
	}
	error = apfs_lookup(fc->vol, parent, name, strlen(name), &id, &dt);
	if (error) {
		apfs_log("fs_remove_file %s in %llu: lookup: %d\n", name, (unsigned long long)parent, error);
		return error;
	}
	error = apfs_inode_get(fc->vol, id, &ino);
	if (error) {
		apfs_log("fs_remove_file %s (%llu): inode_get: %d\n", name, (unsigned long long)id, error);
		return error;
	}
	if (dir && (ino.mode & APFS_S_IFMT) != APFS_S_IFDIR) {
		return ENOTDIR;
	}
	if (!dir && (ino.mode & APFS_S_IFMT) == APFS_S_IFDIR) {
		return EISDIR;
	}
	if (dir && ino.nchildren != 0) {
		/*
		 * The counter says there are names inside; look.  It drifts (a file taken away while
		 * open goes through the orphan's own path), and a directory that is empty but
		 * counts a child can never be removed -- the container manager traps on that
		 * ENOTEMPTY again and again (p177).
		 */
		unsigned inside = 0;
		int rerr;

		rerr = apfs_readdir(fc->vol, id, count_cb, &inside);
		if (inside == 0 && rerr == 0) {
			apfs_log("fs: rmdir %s in %llu: counted %u, none there: taking it\n", name,
			    (unsigned long long)parent, ino.nchildren);
		} else {
			struct list_ctx lc = { 0, 0 };

			(void)apfs_readdir(fc->vol, id, list_cb, &lc);
			apfs_log("fs: rmdir %s in %llu: %u counted, readdir %d, %u names:\n", name,
			    (unsigned long long)parent, ino.nchildren, rerr, lc.total);
			return ENOTEMPTY;
		}
	}
	error = apfs_inode_get(fc->vol, parent, &pino);
	if (error) {
		apfs_log("fs_remove_file %s: parent %llu inode_get: %d\n", name, (unsigned long long)parent, error);
		return error;
	}
	/*
	 * A file with more than one name keeps its inode, its extents and its bytes: only the
	 * name goes, and the link count comes down by one.  The last name is the one that takes
	 * the file itself away.
	 */
	if ((!dir && ino.nchildren > 1) || (!dir && keep)) {
		uint32_t bs = apfs_txn_block_size(fc->txn);
		uint8_t *ival = apfs_alloc(bs);
		uint16_t ivlen;

		if (ival == NULL) {
			return ENOMEM;
		}
		error = apfs_inode_value_get(fc->vol, id, ival, bs, &ivlen);
		if (error == 0 && ino.nchildren > 1) {
			struct other_name o = { id, name, { 0 } };

			(void)apfs_readdir(fc->vol, parent, other_name_cb, &o);
			if (o.name[0] != 0) {
				error = inode_set_name(ival, &ivlen, o.name, strlen(o.name));
			}
		}
		if (error == 0) {
			/*
			 * The last name of a file that is still open falls to zero links and the
			 * file stays: its inode, its extents and its bytes are what the reader that
			 * has it open is still reading.  VNOP_INACTIVE, at the last close, deletes
			 * it (apfs_delete_inode).
			 */
			f_put32(ival + offsetof(struct apfs_j_inode_val, nchildren),
			    (uint32_t)(keep && ino.nchildren <= 1 ? 0 : ino.nchildren - 1));
			f_put64(ival + offsetof(struct apfs_j_inode_val, change_time), when);
			fs_key(key, id, APFS_TYPE_INODE);
			error = btp_update(&fc->fs, BTP_REPLACE, key, 8, ival, ivlen);
		}
		apfs_free(ival, bs);
		if (error) {
			return error;
		}
		klen = drec_key(key, parent, name, strlen(name), fs_fold(fc->vol));
		if (klen == 0) {
			return EINVAL;
		}
		error = btp_update(&fc->fs, BTP_DELETE, key, klen, NULL, 0);
		if (error) {
			return error;
		}
		return fs_write_parent(fc, parent, pino.nchildren - 1, when);
	}
	/* the name's record goes, then the file's */
	error = btp_update(&fc->fs, BTP_DELETE, key, klen, NULL, 0);
	if (error) {
		apfs_log("fs_remove_file %s in %llu: delete drec: %d\n", name, (unsigned long long)parent, error);
		return error;
	}
	error = fs_delete_inode(fc, id, &ino, dir, parent, true, when);
	if (error) {
		apfs_log("fs_remove_file %s (%llu): delete inode: %d\n", name, (unsigned long long)id, error);
	}
	return error;
}

/* ---------------------------------------------------------------- a file's contents */

/* One extended field of an inode record, by type, or NULL. */
static uint8_t *
inode_xfield(uint8_t *val, uint16_t vlen, uint8_t want, uint16_t *out_len)
{
	uint32_t base = (uint32_t)offsetof(struct apfs_j_inode_val, xfields);
	uint16_t num, i, off;

	*out_len = 0;
	if (vlen < base + 4) {
		return NULL;
	}
	num = (uint16_t)(val[base] | (uint16_t)val[base + 1] << 8);
	off = (uint16_t)(base + 4 + 4 * num);
	if ((uint32_t)base + 4 + 4 * (uint32_t)num > vlen) {
		return NULL;
	}
	for (i = 0; i < num; i++) {
		uint8_t t = val[base + 4 + 4 * i];
		uint16_t sz = (uint16_t)(val[base + 4 + 4 * i + 2] |
		    (uint16_t)val[base + 4 + 4 * i + 3] << 8);

		if ((uint32_t)off + sz > vlen) {
			return NULL;
		}
		if (t == want) {
			*out_len = sz;
			return val + off;
		}
		off = (uint16_t)(off + ((sz + 7u) & ~7u));
	}
	return NULL;
}

/* The most extended fields this writer will rebuild an inode record around. */
#define FS_XF_MAX 32

/* One extended field of an inode value, and where its data lies in the value. */
struct xf_slot {
	uint8_t        type;
	uint8_t        flags;
	uint16_t       len;
	const uint8_t *data;
};

/* The extended fields of a record as they are, in the order the record keeps them. */
static int
inode_xf_list(uint8_t *val, uint16_t vlen, struct xf_slot *out, uint16_t *count)
{
	uint32_t base = (uint32_t)offsetof(struct apfs_j_inode_val, xfields);
	uint16_t num, i, off;

	*count = 0;
	if (vlen < base + 4) {
		return EIO;
	}
	num = (uint16_t)(val[base] | (uint16_t)val[base + 1] << 8);
	if ((uint32_t)base + 4 + 4 * (uint32_t)num > vlen || num > FS_XF_MAX) {
		return EIO;
	}
	off = (uint16_t)(base + 4 + 4 * num);
	for (i = 0; i < num; i++) {
		uint16_t sz = (uint16_t)(val[base + 4 + 4 * i + 2] |
		    (uint16_t)val[base + 4 + 4 * i + 3] << 8);

		if ((uint32_t)off + sz > vlen) {
			return EIO;
		}
		out[i].type = val[base + 4 + 4 * i];
		out[i].flags = val[base + 4 + 4 * i + 1];
		out[i].len = sz;
		out[i].data = val + off;
		off = (uint16_t)(off + ((sz + 7u) & ~7u));
	}
	*count = num;
	return 0;
}

/*
 * Put one extended field into an inode record's value: replace the field of that type, or
 * insert it where its type belongs.  The whole blob is rebuilt -- descriptors first, in
 * ascending order of type (apfsck insists), then their data padded out to eight bytes each.
 * That is the only way to change a field that is not the last: the name is first and the
 * data stream last, and either one can be what changes.
 */
static int
inode_xf_apply(uint8_t *val, uint16_t *vlen, uint8_t type, uint8_t flags,
    const uint8_t *data, uint16_t len, bool drop)
{
	struct xf_slot f[FS_XF_MAX + 1];
	uint32_t base = (uint32_t)offsetof(struct apfs_j_inode_val, xfields);
	uint8_t *tmp;
	uint16_t n, i, off, used, padded;
	int error;

	error = inode_xf_list(val, *vlen, f, &n);
	if (error) {
		return error;
	}
	for (i = 0; i < n && f[i].type < type; i++) {
	}
	if (i < n && f[i].type == type) {
		if (drop) {
			memmove(&f[i], &f[i + 1], (size_t)(n - i - 1) * sizeof(f[0]));
			n--;
		} else {
			f[i].flags = flags;
			f[i].len = len;
			f[i].data = data;
		}
	} else if (!drop) {
		if (n == FS_XF_MAX) {
			return ENOSPC;
		}
		memmove(&f[i + 1], &f[i], (size_t)(n - i) * sizeof(f[0]));
		f[i].type = type;
		f[i].flags = flags;
		f[i].len = len;
		f[i].data = data;
		n++;
	}

	off = (uint16_t)(base + 4 + 4 * n);
	used = 0;
	for (i = 0; i < n; i++) {
		used = (uint16_t)(used + ((f[i].len + 7u) & ~7u));
	}
	if ((uint32_t)off + used > 4096) {
		return ENOSPC;
	}

	/*
	 * Into a block of its own first: the data area moves when a field gets longer or
	 * shorter, and a copy in place would read what it has just written.
	 */
	tmp = apfs_alloc(4096);
	if (tmp == NULL) {
		return ENOMEM;
	}
	memcpy(tmp, val, base);
	for (i = 0; i < n; i++) {
		padded = (uint16_t)((f[i].len + 7u) & ~7u);
		tmp[base + 4 + 4 * i] = f[i].type;
		tmp[base + 4 + 4 * i + 1] = f[i].flags;
		f_put16(tmp + base + 4 + 4 * i + 2, f[i].len);
		memset(tmp + off, 0, padded);
		if (f[i].len != 0) {
			memcpy(tmp + off, f[i].data, f[i].len);
		}
		off = (uint16_t)(off + padded);
	}
	tmp[base] = (uint8_t)n;
	tmp[base + 1] = (uint8_t)(n >> 8);
	f_put16(tmp + base + 2, used);
	memcpy(val, tmp, off);
	*vlen = off;
	apfs_free(tmp, 4096);
	return 0;
}

static int
inode_xfield_set(uint8_t *val, uint16_t *vlen, uint8_t type, uint8_t flags,
    const uint8_t *data, uint16_t len)
{
	return inode_xf_apply(val, vlen, type, flags, data, len, false);
}

/* Take one extended field out of an inode record's value, if it is there. */
static int
inode_xfield_del(uint8_t *val, uint16_t *vlen, uint8_t type)
{
	return inode_xf_apply(val, vlen, type, 0, NULL, 0, true);
}

/* The name an inode record says it has: a file's primary name lives here, not only in the
 * directory entry that points at it, and apfsck asks for it. */
static int
inode_set_name(uint8_t *val, uint16_t *vlen, const char *name, size_t n)
{
	if (n == 0 || n >= FS_NAME_MAX) {
		return EINVAL;
	}
	return inode_xfield_set(val, vlen, APFS_INO_EXT_TYPE_NAME, APFS_XF_DO_NOT_COPY,
	    (const uint8_t *)name, (uint16_t)(n + 1));      /* with the terminating zero */
}

/* The size and the allocation of a file's data stream, in its inode's record. */
static int
inode_set_dstream(uint8_t *val, uint16_t *vlen, uint64_t size, uint64_t alloced)
{
	uint8_t ds[40], *old;
	uint16_t old_len;

	memset(ds, 0, sizeof(ds));
	old = inode_xfield(val, *vlen, APFS_INO_EXT_TYPE_DSTREAM, &old_len);
	if (old != NULL && old_len >= sizeof(ds)) {
		memcpy(ds, old, sizeof(ds));    /* the crypto identifier and the counters stay */
	}
	f_put64(ds, size);
	f_put64(ds + 8, alloced);
	return inode_xfield_set(val, vlen, APFS_INO_EXT_TYPE_DSTREAM, APFS_XF_SYSTEM_FIELD,
	    ds, sizeof(ds));
}

/* How much of the file is holes, for the inode's sparse-bytes xfield. */
static int
inode_set_sparse(uint8_t *val, uint16_t *vlen, uint64_t sparse)
{
	uint64_t flags = f_le64(val + offsetof(struct apfs_j_inode_val, internal_flags));
	int error;

	if (sparse != 0) {
		uint8_t v[8];

		f_put64(v, sparse);
		error = inode_xfield_set(val, vlen, APFS_INO_EXT_TYPE_SPARSE_BYTES,
		    APFS_XF_SYSTEM_FIELD | APFS_XF_CHILDREN_INHERIT, v, sizeof(v));
		flags |= APFS_INODE_IS_SPARSE;
	} else {
		error = inode_xfield_del(val, vlen, APFS_INO_EXT_TYPE_SPARSE_BYTES);
		flags &= ~(uint64_t)APFS_INODE_IS_SPARSE;
	}
	f_put64(val + offsetof(struct apfs_j_inode_val, internal_flags), flags);
	return error;
}

/* One run of data blocks: its file-extent record and its entry in the reference tree. */
static int
fs_emit_run(struct fs_ctx *fc, uint64_t sid, uint64_t off, uint64_t phys, uint32_t blocks)
{
	uint32_t bs = apfs_txn_block_size(fc->txn);
	uint8_t key[16], val[24], rkey[8], rval[20];
	int error;

	fs_key(key, sid, APFS_TYPE_FILE_EXTENT);
	f_put64(key + 8, off);
	f_put64(val, (uint64_t)blocks * bs);
	f_put64(val + 8, phys);
	f_put64(val + 16, 0);
	error = btp_update(&fc->fs, BTP_REPLACE, key, 16, val, 24);
	if (error == ENOENT) {
		error = btp_update(&fc->fs, BTP_INSERT, key, 16, val, 24);
	}
	if (error) {
		return error;
	}
	if (fc->has_extref) {
		fs_key(rkey, phys, APFS_TYPE_EXTENT);
		f_put64(rval, (uint64_t)blocks | (1ULL << APFS_OBJ_TYPE_SHIFT));
		f_put64(rval + 8, sid);
		f_put32(rval + 16, 1);
		error = btp_update(&fc->extref, BTP_REPLACE, rkey, 8, rval, 20);
		if (error == ENOENT) {
			error = btp_update(&fc->extref, BTP_INSERT, rkey, 8, rval, 20);
		}
		if (error) {
			return error;
		}
	}
	return 0;
}

/* ---------------------------------------------------------------- rename */

/* The kind a directory entry says a file is, from its mode. */
static uint8_t
fs_dtype(uint16_t mode)
{
	switch (mode & APFS_S_IFMT) {
	case APFS_S_IFDIR:
		return APFS_DT_DIR;
	case APFS_S_IFLNK:
		return APFS_DT_LNK;
	case APFS_S_IFIFO:
		return APFS_DT_FIFO;
	case APFS_S_IFCHR:
		return APFS_DT_CHR;
	case APFS_S_IFBLK:
		return APFS_DT_BLK;
	case APFS_S_IFSOCK:
		return APFS_DT_SOCK;
	default:
		return APFS_DT_REG;
	}
}

/* ---------------------------------------------------------------- hard link */

struct link_arg {
	uint64_t parent;
	uint64_t obj_id;
	char     name[FS_NAME_MAX];
};

/*
 * A hard link is a second name for an inode that stays where it is: one directory entry
 * more, the inode's link count up by one, and the parent's child count with it.  Only a
 * file takes one; a directory's name is its place in the tree.
 */
static int
op_link(struct fs_ctx *fc, void *arg)
{
	struct link_arg *a = arg;
	struct apfs_inode ino, pino;
	uint64_t other = 0, when = apfs_txn_xid(fc->txn);
	uint8_t *ival = NULL;
	uint16_t ivlen, klen;
	uint32_t bs = apfs_txn_block_size(fc->txn);
	uint8_t key[FS_NAME_MAX + 16], val[32];
	int error;

	error = apfs_lookup(fc->vol, a->parent, a->name, strlen(a->name), &other, NULL);
	if (error == 0) {
		return EEXIST;
	}
	if (error != ENOENT) {
		return error;
	}
	error = apfs_inode_get(fc->vol, a->obj_id, &ino);
	if (error) {
		return error;
	}
	if ((ino.mode & APFS_S_IFMT) == APFS_S_IFDIR) {
		return EPERM;
	}
	error = apfs_inode_get(fc->vol, a->parent, &pino);
	if (error) {
		return error;
	}
	if ((pino.mode & APFS_S_IFMT) != APFS_S_IFDIR) {
		return ENOTDIR;
	}

	ival = apfs_alloc(bs);
	if (ival == NULL) {
		return ENOMEM;
	}
	error = apfs_inode_value_get(fc->vol, a->obj_id, ival, bs, &ivlen);
	if (error) {
		goto out;
	}
	f_put32(ival + offsetof(struct apfs_j_inode_val, nchildren),
	    (uint32_t)(ino.nchildren + 1));
	f_put64(ival + offsetof(struct apfs_j_inode_val, change_time), when);
	fs_key(key, a->obj_id, APFS_TYPE_INODE);
	error = btp_update(&fc->fs, BTP_REPLACE, key, 8, ival, ivlen);
	if (error) {
		goto out;
	}

	klen = drec_key(key, a->parent, a->name, strlen(a->name), fs_fold(fc->vol));
	if (klen == 0) {
		error = EINVAL;
		goto out;
	}
	f_put64(val, a->obj_id);
	f_put64(val + 8, when);
	f_put16(val + 16, fs_dtype(ino.mode));
	error = btp_update(&fc->fs, BTP_INSERT, key, klen, val, 18);
	if (error) {
		goto out;
	}
	error = fs_write_parent(fc, a->parent, pino.nchildren + 1, when);
out:
	apfs_free(ival, bs);
	return error;
}

struct rename_arg {
	uint64_t parent;
	uint64_t new_parent;
	char     name[FS_NAME_MAX];
	char     new_name[FS_NAME_MAX];
};

/*
 * A rename is a directory entry taken from one name and put under another: the name in the
 * inode's own record changes with it, and so does the parent it points at.  Nothing else of
 * the file moves; its identifier, its blocks and its data stream stay as they are.
 */
static int
op_rename(struct fs_ctx *fc, void *arg)
{
	struct rename_arg *a = arg;
	struct apfs_inode ino, pino, pnew, walk;
	uint64_t id = 0, other = 0, when = apfs_txn_xid(fc->txn);
	uint8_t dt = 0, newdt = 0, *ival = NULL;
	uint16_t ivlen, klen;
	uint32_t bs = apfs_txn_block_size(fc->txn);
	uint8_t key[FS_NAME_MAX + 16], val[32];
	bool same_parent = (a->parent == a->new_parent);
	int error;

	error = apfs_lookup(fc->vol, a->parent, a->name, strlen(a->name), &id, &dt);
	if (error) {
		return error;
	}
	if (same_parent && strcmp(a->name, a->new_name) == 0) {
		return 0;               /* the same name in the same place: nothing to do */
	}
	error = apfs_inode_get(fc->vol, id, &ino);
	if (error) {
		return error;
	}
	/* the new name has to be free, unless it is the name this entry already has */
	error = apfs_lookup(fc->vol, a->new_parent, a->new_name, strlen(a->new_name), &other,
	    &newdt);
	if (error == 0 && other != id) {
		/*
		 * The vnop takes the old entry away first (apfs_remove at the VFS layer): removing
		 * it here, inside the same transaction, is where the kernel's stack went bad
		 * (p176: "Invalid kernel stack pointer" in pwd_mkdb's first replace-rename).
		 */
		return EEXIST;
	}
	if (error != 0 && error != ENOENT) {
		return error;
	}
	error = apfs_inode_get(fc->vol, a->parent, &pino);
	if (error) {
		return error;
	}
	if (!same_parent) {
		error = apfs_inode_get(fc->vol, a->new_parent, &pnew);
		if (error) {
			return error;
		}
		if ((pnew.mode & APFS_S_IFMT) != APFS_S_IFDIR) {
			return ENOTDIR;
		}
	}
	/* a directory may not go inside itself, or the tree would close a loop */
	if ((ino.mode & APFS_S_IFMT) == APFS_S_IFDIR) {
		uint64_t p = a->new_parent;
		int depth;

		for (depth = 0; depth < 128; depth++) {
			if (p == id) {
				return EINVAL;
			}
			if (p == APFS_ROOT_DIR_INO_NUM) {
				break;
			}
			error = apfs_inode_get(fc->vol, p, &walk);
			if (error) {
				return error;
			}
			p = walk.parent_id;
		}
	}

	ival = apfs_alloc(bs);
	if (ival == NULL) {
		return ENOMEM;
	}
	error = apfs_inode_value_get(fc->vol, id, ival, bs, &ivlen);
	if (error) {
		goto out;
	}
	error = inode_set_name(ival, &ivlen, a->new_name, strlen(a->new_name));
	if (error) {
		goto out;
	}
	f_put64(ival + offsetof(struct apfs_j_inode_val, parent_id), a->new_parent);
	f_put64(ival + offsetof(struct apfs_j_inode_val, change_time), when);

	/* the old entry out, the new one in: the key carries the name, the value the file */
	klen = drec_key(key, a->parent, a->name, strlen(a->name), fs_fold(fc->vol));
	if (klen == 0) {
		error = EINVAL;
		goto out;
	}
	error = btp_update(&fc->fs, BTP_DELETE, key, klen, NULL, 0);
	if (error) {
		goto out;
	}
	klen = drec_key(key, a->new_parent, a->new_name, strlen(a->new_name), fs_fold(fc->vol));
	if (klen == 0) {
		error = EINVAL;
		goto out;
	}
	f_put64(val, id);
	f_put64(val + 8, when);
	f_put16(val + 16, dt != 0 ? dt : fs_dtype(ino.mode));
	error = btp_update(&fc->fs, BTP_INSERT, key, klen, val, 18);
	if (error) {
		goto out;
	}

	fs_key(key, id, APFS_TYPE_INODE);
	error = btp_update(&fc->fs, BTP_REPLACE, key, 8, ival, ivlen);
	if (error) {
		goto out;
	}

	/* one child leaves one directory and arrives in the other */
	if (same_parent) {
		error = fs_write_parent(fc, a->parent, pino.nchildren, when);
	} else {
		error = fs_write_parent(fc, a->parent, pino.nchildren - 1, when);
		if (error == 0) {
			error = fs_write_parent(fc, a->new_parent, pnew.nchildren + 1, when);
		}
	}
out:
	apfs_free(ival, bs);
	return error;
}

/* ---------------------------------------------------------------- truncate */

static int fs_holes_fill(struct fs_ctx *fc, uint64_t sid, uint64_t size);
static int fs_extent_total(struct fs_ctx *fc, uint64_t sid, uint64_t *total);
static int fs_extent_sparse(struct fs_ctx *fc, uint64_t sid, uint64_t *sparse);

struct fs_ext_list {
	struct apfs_extent *e;
	uint32_t            n;
	uint32_t            cap;
};
static int fs_ext_list_load(struct fs_ctx *fc, uint64_t sid, struct fs_ext_list *l);
static int fs_ext_list_finish(struct fs_ctx *fc, uint64_t sid, struct fs_ext_list *l,
    uint64_t size, uint64_t *total, uint64_t *sparse);
static uint32_t fs_ext_list_refs(const struct fs_ext_list *l);
static void fs_ext_list_free(struct fs_ext_list *l);

struct truncate_arg {
	uint64_t obj_id;
	uint64_t size;
};

/* Give one extent's blocks back and take its record out of both trees. */
static int
fs_drop_extent(struct fs_ctx *fc, uint64_t sid, const struct apfs_extent *ex)
{
	uint32_t bs = apfs_txn_block_size(fc->txn);
	uint8_t ekey[16], rkey[8];
	uint32_t nb = (uint32_t)(ex->length / bs), j;
	int error;

	if (ex->phys != 0) {
		for (j = 0; j < nb; j++) {
			error = apfs_txn_free_block_who(fc->txn, ex->phys + j, APFS_FREE_EXTENT, sid);
			if (error) {
				return error;
			}
		}
		fc->data_delta -= (int64_t)nb;
		if (fc->has_extref) {
			fs_key(rkey, ex->phys, APFS_TYPE_EXTENT);
			error = btp_update(&fc->extref, BTP_DELETE, rkey, 8, NULL, 0);
			if (error != 0 && error != ENOENT) {
				return error;
			}
		}
	}
	fs_key(ekey, sid, APFS_TYPE_FILE_EXTENT);
	f_put64(ekey + 8, ex->logical);
	error = btp_update(&fc->fs, BTP_DELETE, ekey, 16, NULL, 0);
	if (error != 0 && error != ENOENT) {
		return error;
	}
	return 0;
}

/*
 * Cutting a file short.  The extents past the new size go, blocks and reference records
 * with them; the extent the file now ends in stays, and only its tail is given back -- a
 * file cut in the middle of a block keeps that block, and its data stream says so.
 */
static int
op_truncate(struct fs_ctx *fc, void *arg)
{
	struct truncate_arg *a = arg;
	uint32_t bs = apfs_txn_block_size(fc->txn);
	struct apfs_inode ino;
	struct apfs_extent ex, keep;
	uint8_t *ival = NULL;
	uint16_t ivlen;
	uint8_t key[16], val[8];
	uint64_t alloced, at, trim_at, sid, when = apfs_txn_xid(fc->txn);
	uint64_t sparse = 0;
	uint32_t refs = 0;
	bool have_keep = false;
	int error;

	error = apfs_inode_get(fc->vol, a->obj_id, &ino);
	if (error) {
		return error;
	}
	if ((ino.mode & APFS_S_IFMT) == APFS_S_IFDIR) {
		return EISDIR;
	}
	if (a->size > ino.size) {
		/*
		 * Growing is only the size in the inode: what lies between the old end and the
		 * new one is a hole, and a hole reads as zeros.  No block is allocated and no
		 * extent record is made.
		 */
		ival = apfs_alloc(bs);
		if (ival == NULL) {
			return ENOMEM;
		}
		{
			struct fs_ext_list l;
			uint64_t total = 0, sparse = 0;

			error = fs_ext_list_load(fc, ino.private_id, &l);
			if (error == 0) {
				error = fs_ext_list_finish(fc, ino.private_id, &l, a->size,
				    &total, &sparse);
				refs = fs_ext_list_refs(&l);
				fs_ext_list_free(&l);
			}
			if (error == 0) {
				error = apfs_inode_value_get(fc->vol, a->obj_id, ival, bs, &ivlen);
			}
			if (error == 0) {
				error = inode_set_dstream(ival, &ivlen, a->size, total);
			}
			if (error == 0) {
				error = inode_set_sparse(ival, &ivlen, sparse);
			}
		}
		if (error == 0) {
			f_put64(ival + offsetof(struct apfs_j_inode_val, mod_time), when);
			f_put64(ival + offsetof(struct apfs_j_inode_val, change_time), when);
			fs_key(key, a->obj_id, APFS_TYPE_INODE);
			error = btp_update(&fc->fs, BTP_REPLACE, key, 8, ival, ivlen);
		}
		if (error == 0 && a->size > 0) {
			fs_key(key, ino.private_id, APFS_TYPE_DSTREAM_ID);
			f_put32(val, 1);
			error = btp_update(&fc->fs, BTP_REPLACE, key, 8, val, 4);
			if (error == ENOENT) {
				error = btp_update(&fc->fs, BTP_INSERT, key, 8, val, 4);
			}
		}
		apfs_free(ival, bs);
		return error;
	}
	if (a->size == ino.size) {
		return 0;
	}
	sid = ino.private_id;
	alloced = ino.alloced_size;
	trim_at = ((a->size + bs - 1) / bs) * bs;
	/*
	 * The holes the stream has before this cut, from the extents as they are now: every
	 * record given back below takes its length off this number.  Reading them again after
	 * the changes would not do -- the tree the library reads is the one on the disk, not
	 * what this transaction has staged so far.
	 */
	error = fs_extent_sparse(fc, sid, &sparse);
	if (error) {
		return error;
	}

	/*
	 * Where the file now ends: the extent that covers the last byte it keeps.  It stays,
	 * and its blocks after trim_at are given back below; everything after it goes whole.
	 */
	if (a->size > 0) {
		error = apfs_extent_at(fc->vol, sid, a->size - 1, &keep);
		if (error) {
			return error;
		}
		have_keep = true;
		at = keep.logical + keep.length;
	} else {
		at = 0;
	}
	for (;;) {
		error = apfs_extent_next(fc->vol, sid, at, &ex);
		if (error == ENOENT) {
			break;          /* past the last extent */
		}
		if (error) {
			return error;
		}
		error = fs_drop_extent(fc, sid, &ex);
		if (error) {
			return error;
		}
		alloced -= ex.length;
		if (ex.phys == 0) {
			sparse -= ex.length;
		}
		if (ex.length == 0) {
			break;
		}
		at = ex.logical + ex.length;
	}

	if (have_keep && keep.phys != 0) {
		uint64_t keep_blocks = (trim_at - keep.logical) / bs;
		uint64_t have_blocks = keep.length / bs;

		/*
		 * A file cut in the middle of a block keeps that block, and what lies past the new
		 * end in it is no longer the file: zero it here, or extending the file again would
		 * bring the old bytes back as if they had never been cut.
		 */
		if ((a->size % bs) != 0) {
			uint64_t at = keep.phys + (a->size - 1 - keep.logical) / bs;
			const uint8_t *cur = NULL;
			uint8_t *tmp = apfs_alloc(bs);

			if (tmp == NULL) {
				return ENOMEM;
			}
			error = apfs_block_read_raw(fc->vol->nx, at, &cur);
			if (error == 0) {
				memcpy(tmp, cur, bs);
				memset(tmp + (a->size % bs), 0, bs - (a->size % bs));
				error = apfs_txn_stage_block(fc->txn, at, tmp);
			}
			apfs_free(tmp, bs);
			if (error) {
				return error;
			}
		}

		if (keep_blocks == 0) {
			error = fs_drop_extent(fc, sid, &keep);
			if (error) {
				return error;
			}
			alloced -= keep.length;
		} else if (keep_blocks < have_blocks) {
			uint32_t nb = (uint32_t)(have_blocks - keep_blocks), j;
			uint8_t val[24], rkey[8], rval[20];

			for (j = 0; j < nb; j++) {
				error = apfs_txn_free_block_who(fc->txn, keep.phys + keep_blocks + j,
				    APFS_FREE_TRUNC, sid);
				if (error) {
					return error;
				}
			}
			fc->data_delta -= (int64_t)nb;
			alloced -= (uint64_t)nb * bs;
			/* the record now covers only what is kept */
			fs_key(key, sid, APFS_TYPE_FILE_EXTENT);
			f_put64(key + 8, keep.logical);
			f_put64(val, keep_blocks * bs);
			f_put64(val + 8, keep.phys);
			f_put64(val + 16, 0);
			error = btp_update(&fc->fs, BTP_REPLACE, key, 16, val, 24);
			if (error) {
				return error;
			}
			if (fc->has_extref) {
				fs_key(rkey, keep.phys, APFS_TYPE_EXTENT);
				f_put64(rval, keep_blocks | (1ULL << APFS_OBJ_TYPE_SHIFT));
				f_put64(rval + 8, sid);
				f_put32(rval + 16, 1);
				error = btp_update(&fc->extref, BTP_REPLACE, rkey, 8, rval, 20);
				if (error) {
					return error;
				}
			}
		}
	} else if (have_keep) {
		/*
		 * The file ends in a hole: its record covers the block the file now ends in and
		 * nothing after it.  Left as it was, the record would reach past the end of the
		 * file, and the holes it no longer covers would still be counted as sparse bytes.
		 */
		uint64_t hole_len = trim_at - keep.logical;

		if (hole_len == 0) {
			error = fs_drop_extent(fc, sid, &keep);
			if (error) {
				return error;
			}
		} else if (hole_len < keep.length) {
			uint8_t val[24];

			fs_key(key, sid, APFS_TYPE_FILE_EXTENT);
			f_put64(key + 8, keep.logical);
			f_put64(val, hole_len);
			f_put64(val + 8, 0);
			f_put64(val + 16, 0);
			error = btp_update(&fc->fs, BTP_REPLACE, key, 16, val, 24);
			if (error) {
				return error;
			}
			sparse -= keep.length - hole_len;
			alloced -= keep.length - hole_len;
		}
	}

	ival = apfs_alloc(bs);
	if (ival == NULL) {
		return ENOMEM;
	}
	error = apfs_inode_value_get(fc->vol, a->obj_id, ival, bs, &ivlen);
	if (error) {
		goto out;
	}
	/*
	 * A file with no bytes has no data stream at all: apfsck wants the stream's record
	 * and the reference count that goes with it to be absent together.
	 */
	if (a->size == 0) {
		error = inode_xfield_del(ival, &ivlen, APFS_INO_EXT_TYPE_DSTREAM);
		/* no stream, no holes in it: the two fields go together */
		if (error == 0) {
			error = inode_set_sparse(ival, &ivlen, 0);
		}
	} else {
		error = inode_set_dstream(ival, &ivlen, a->size, alloced);
		/*
		 * The holes the stream has left, in one number: a cut through a hole changes the
		 * extents just as a write does, so both go through the same account.
		 */
		if (error == 0) {
			error = inode_set_sparse(ival, &ivlen, sparse);
		}
	}
	if (error) {
		goto out;
	}
	f_put64(ival + offsetof(struct apfs_j_inode_val, mod_time), when);
	f_put64(ival + offsetof(struct apfs_j_inode_val, change_time), when);
	fs_key(key, a->obj_id, APFS_TYPE_INODE);
	error = btp_update(&fc->fs, BTP_REPLACE, key, 8, ival, ivlen);
	if (error) {
		goto out;
	}
	/* an empty file has no stream to keep a record for */
	if (a->size == 0) {
		fs_key(key, sid, APFS_TYPE_DSTREAM_ID);
		error = btp_update(&fc->fs, BTP_DELETE, key, 8, NULL, 0);
		if (error == ENOENT) {
			error = 0;
		}
	}
out:
	apfs_free(ival, bs);
	return error;
}

/* ---------------------------------------------------------------- extended attributes */

struct xattr_arg {
	uint64_t       obj_id;
	char           name[FS_NAME_MAX];
	const uint8_t *data;
	size_t         len;
	uint16_t       flags;
	bool           remove;
};

/* An attribute's key: the object, the kind, the name's length (with its zero) and the name. */
static uint16_t
xattr_key(uint8_t *out, uint64_t obj_id, const char *name, size_t name_len)
{
	size_t i;

	if (name_len == 0 || name_len >= FS_NAME_MAX) {
		return 0;
	}
	fs_key(out, obj_id, APFS_TYPE_XATTR);
	f_put16(out + 8, (uint16_t)(name_len + 1));
	for (i = 0; i < name_len; i++) {
		out[10 + i] = (uint8_t)name[i];
	}
	out[10 + name_len] = 0;
	return (uint16_t)(10 + name_len + 1);
}

/*
 * One attribute of an object, set or taken away.  The value lives in its record while it is
 * small enough for that (APFS_XATTR_MAX_EMBEDDED_SIZE); a value with a stream of its own is
 * not made here.
 */
static int
op_xattr(struct fs_ctx *fc, void *arg)
{
	struct xattr_arg *a = arg;
	struct apfs_inode ino;
	uint8_t key[FS_NAME_MAX + 16], *val = NULL, *ival = NULL;
	uint16_t klen, ivlen;
	uint32_t bs = apfs_txn_block_size(fc->txn);
	uint64_t when = apfs_txn_xid(fc->txn);
	int error;

	error = apfs_inode_get(fc->vol, a->obj_id, &ino);
	if (error) {
		return error;
	}
	klen = xattr_key(key, a->obj_id, a->name, strlen(a->name));
	if (klen == 0) {
		return EINVAL;
	}
	if (a->remove) {
		error = btp_update(&fc->fs, BTP_DELETE, key, klen, NULL, 0);
		if (error != 0) {
			return error;
		}
	} else {
		if (a->len > APFS_XATTR_MAX_EMBEDDED_SIZE) {
			return E2BIG;
		}
		val = apfs_alloc((size_t)a->len + 4);
		if (val == NULL) {
			return ENOMEM;
		}
		f_put16(val, (uint16_t)(APFS_XATTR_DATA_EMBEDDED | a->flags));
		f_put16(val + 2, (uint16_t)a->len);
		if (a->len != 0) {
			memcpy(val + 4, a->data, a->len);
		}
		error = btp_update(&fc->fs, BTP_REPLACE, key, klen, val, (uint16_t)(a->len + 4));
		if (error == ENOENT) {
			error = btp_update(&fc->fs, BTP_INSERT, key, klen, val,
			    (uint16_t)(a->len + 4));
		}
		apfs_free(val, (size_t)a->len + 4);
		if (error) {
			return error;
		}
	}

	/* the inode's change time moves with an attribute, as it does on any Unix */
	ival = apfs_alloc(bs);
	if (ival == NULL) {
		return ENOMEM;
	}
	error = apfs_inode_value_get(fc->vol, a->obj_id, ival, bs, &ivlen);
	if (error == 0) {
		f_put64(ival + offsetof(struct apfs_j_inode_val, change_time), when);
		/*
		 * A compressed file is a decmpfs attribute that says so; with the attribute
		 * gone the inode must not go on claiming it is compressed (apfsck: "Compressed
		 * file: missing decmpfs xattr").  The uncompressed-size field and UF_COMPRESSED
		 * are the two ways it claims that.
		 */
		if (a->remove && strcmp(a->name, APFS_DECMPFS_XATTR_NAME) == 0) {
			uint64_t flags = f_le64(ival + offsetof(struct apfs_j_inode_val,
			    internal_flags));
			uint32_t bsd = f_le32(ival + offsetof(struct apfs_j_inode_val, bsd_flags));

			f_put64(ival + offsetof(struct apfs_j_inode_val, internal_flags),
			    flags & ~(uint64_t)APFS_INODE_HAS_UNCOMPRESSED_SIZE);
			f_put32(ival + offsetof(struct apfs_j_inode_val, bsd_flags),
			    bsd & ~(uint32_t)0x20);         /* UF_COMPRESSED */
		}
		fs_key(key, a->obj_id, APFS_TYPE_INODE);
		error = btp_update(&fc->fs, BTP_REPLACE, key, 8, ival, ivlen);
	}
	apfs_free(ival, bs);
	return error;
}

/* ---------------------------------------------------------------- the whole operation */

/*
 * Everything after the records: the object map entries, the volume superblock, the container's
 * map entry for that superblock, and the commit.  The same for a create, an unlink and a change
 * of one record.
 */
static int
fs_commit(struct fs_ctx *fc, uint64_t when)
{
	struct apfs_vol *vol = fc->vol;
	struct apfs_nx *nx = vol->nx;
	struct apfs_txn *txn = fc->txn;
	struct btp_tree nxtree;
	uint8_t *vobj = NULL, *nobj = NULL, *blk = NULL;
	uint64_t fs_root = 0, omap_root = 0, vsb_addr = 0, omap_obj_addr = 0;
	uint64_t nx_root = 0, nx_obj_addr = 0, extref_root = 0;
	apfs_oid_t dummy_oid = 0;
	uint8_t key[16], val[16];
	uint32_t bs = apfs_txn_block_size(txn);
	uint64_t old_omap_obj = vol->omap_addr, old_sb = vol->sb_addr;
	uint64_t old_nx_obj = nx->omap_addr;
	uint64_t old_nx_next_oid = f_le64(nx->sb + 88);
	int error;

	error = fs_publish(fc);
	if (error) {
		apfs_log("fs: publish: %d\n", error);
	}
	if (error == 0) {
		error = btp_root(&fc->fs, &fs_root, &dummy_oid);
	}
	if (error == 0) {
		error = btp_root(&fc->omap, &omap_root, &dummy_oid);
	}
	if (error) {
		return error;
	}

	/* the volume's object map object: the tree it names, sealed at its new block */
	error = apfs_txn_copy_block(txn, old_omap_obj, &vobj);
	if (error) {
		return error;
	}
	error = apfs_txn_alloc_block(txn, &omap_obj_addr);
	if (error) {
		apfs_free(vobj, bs);
		return error;
	}
	f_put64(vobj + 48, omap_root);
	apfs_txn_seal(vobj, bs, omap_obj_addr, apfs_txn_xid(txn),
	    APFS_OBJ_PHYSICAL | APFS_OBJECT_TYPE_OMAP, 0);
	error = apfs_txn_stage_block(txn, omap_obj_addr, vobj);
	apfs_free(vobj, bs);
	if (error) {
		return error;
	}

	/* the volume superblock: that map, the times, and whatever the operation counted */
	if (btp_next_oid(&fc->fs) > fs_next_obj_id(fc)) {
		fs_set_next_obj_id(fc, btp_next_oid(&fc->fs));
	}
	/*
	 * The identifiers are one space for the whole container: apfsck checks that every
	 * object's oid is below the *container* superblock's next_oid, not only the volume's
	 * next_obj_id.  Without this a create leaves the container's counter behind and every
	 * new node is "an unassigned object id".
	 */
	if (btp_next_oid(&fc->fs) > f_le64(nx->sb + 88)) {
		f_put64(nx->sb + 88, btp_next_oid(&fc->fs));
	}
	/*
	 * The volume's block count is every node of its two trees and every block of its
	 * files, and apfsck adds them up from the trees; a split adds one node here.
	 */
	{
		int64_t delta = (int64_t)(btp_node_count(&fc->fs) - vol->tree.node_count) +
		    (int64_t)(btp_node_count(&fc->omap) - vol->omap.tree.node_count);

		if (fc->has_extref) {
			delta += (int64_t)(btp_node_count(&fc->extref) - fc->extref_nodes_old);
		}
		delta += fc->data_delta;
		f_put64(fc->sb + 0x58, f_le64(fc->sb + 0x58) + (uint64_t)delta);
	}
	error = apfs_txn_alloc_block(txn, &vsb_addr);
	if (error) {
		return error;
	}
	blk = apfs_alloc(bs);
	if (blk == NULL) {
		return ENOMEM;
	}
	memcpy(blk, fc->sb, bs);
	f_put64(blk + 128, omap_obj_addr);              /* apfs_omap_oid */
	if (fc->has_extref) {
		uint64_t er_root = 0;
		apfs_oid_t er_oid = 0;

		error = btp_root(&fc->extref, &er_root, &er_oid);
		if (error) {
			apfs_free(blk, bs);
			return error;
		}
		f_put64(blk + 0x90, er_root);           /* apfs_extentref_tree_oid */
		extref_root = er_root;
	}
	f_put64(blk + 0x100, when);                     /* apfs_last_mod_time */
	f_put64(blk + 0xe0, f_le64(blk + 0xe0) + 1);    /* apfs_total_blocks_alloced */
	apfs_txn_seal(blk, bs, vol->oid, apfs_txn_xid(txn),
	    APFS_OBJ_VIRTUAL | APFS_OBJECT_TYPE_FS, 0);
	error = apfs_txn_stage_block(txn, vsb_addr, blk);
	apfs_free(blk, bs);
	if (error) {
		return error;
	}

	/* the container's object map: an entry for that superblock, keyed by this transaction */
	error = btp_open(&nxtree, txn, nx->omap.tree.root, 0, true, 16, 16, 0x10,
	    APFS_OBJECT_TYPE_OMAP, omap_cmp, NULL, NULL, 0);
	if (error) {
		return error;
	}
	/* the superblock's old record is retired with the block it named */
	{
		apfs_xid_t old_xid = 0;
		uint64_t old_addr = 0;
		uint8_t old_key[16];

		if (apfs_omap_lookup_key(&nx->omap, vol->oid, apfs_txn_xid(txn), &old_addr,
		    &old_xid) == 0 && old_xid != 0 && old_xid != apfs_txn_xid(txn)) {
			f_put64(old_key, vol->oid);
			f_put64(old_key + 8, old_xid);
			error = btp_update(&nxtree, BTP_DELETE, old_key, 16, NULL, 0);
			if (error != 0 && error != ENOENT) {
				btp_close(&nxtree);
				return error;
			}
		}
	}
	f_put64(key, vol->oid);
	f_put64(key + 8, apfs_txn_xid(txn));
	f_put32(val, 0);
	f_put32(val + 4, bs);
	f_put64(val + 8, vsb_addr);
	error = btp_update(&nxtree, BTP_REPLACE, key, 16, val, 16);
	if (error == ENOENT) {
		error = btp_update(&nxtree, BTP_INSERT, key, 16, val, 16);
	}
	if (error == 0) {
		error = btp_root(&nxtree, &nx_root, &dummy_oid);
	}
	if (error) {
		btp_close(&nxtree);
		return error;
	}
	btp_close(&nxtree);

	/* the container's map object: the tree it names */
	error = apfs_txn_copy_block(txn, old_nx_obj, &nobj);
	if (error) {
		return error;
	}
	error = apfs_txn_alloc_block(txn, &nx_obj_addr);
	if (error) {
		apfs_free(nobj, bs);
		return error;
	}
	f_put64(nobj + 48, nx_root);
	apfs_txn_seal(nobj, bs, nx_obj_addr, apfs_txn_xid(txn),
	    APFS_OBJ_PHYSICAL | APFS_OBJECT_TYPE_OMAP, 0);
	error = apfs_txn_stage_block(txn, nx_obj_addr, nobj);
	apfs_free(nobj, bs);
	if (error) {
		return error;
	}

	/* the blocks the trees gave up are already queued; these two objects are not in a tree */
	if (apfs_txn_free_block_who(txn, old_omap_obj, APFS_FREE_COMMIT, 6) != 0 ||
	    apfs_txn_free_block_who(txn, old_sb, APFS_FREE_COMMIT, 7) != 0 ||
	    apfs_txn_free_block_who(txn, old_nx_obj, APFS_FREE_COMMIT, 8) != 0) {
		return EIO;
	}

	/* the container's superblock names the new map object; the commit does the rest */
	nx->omap_addr = nx_obj_addr;
	error = apfs_txn_commit(txn);
	if (error) {
		nx->omap_addr = old_nx_obj;
		f_put64(nx->sb + 88, old_nx_next_oid);
		return error;
	}
	vol->tree.root = fs_root;
	vol->tree.node_count = btp_node_count(&fc->fs);
	vol->omap.tree.root = omap_root;
	vol->omap.tree.node_count = btp_node_count(&fc->omap);
	vol->omap_addr = omap_obj_addr;
	vol->sb_addr = vsb_addr;
	/*
	 * The extent-reference tree is a physical tree, so the new superblock names its new root
	 * by address -- and the mount's copy of that address has to move with it.  It did not:
	 * the next transaction opened the tree at the root this one had just given back, freed it
	 * a second time, and the allocator had meanwhile handed the block to another tree.
	 */
	if (fc->has_extref) {
		vol->extentref_oid = extref_root;
	}
	nx->omap.tree.root = nx_root;
	return 0;
}

/* ----------------------------------------------------------------- public operations */

static int
fs_run(struct apfs_vol *vol, int (*op)(struct fs_ctx *, void *), void *arg, uint64_t when)
{
	struct apfs_spaceman sm;
	struct apfs_txn *txn;
	struct fs_ctx fc;
	int error;

	if (vol->nx->dev.write == NULL) {
		return EROFS;
	}
	if ((vol->incompat & APFS_INCOMPAT_SEALED_VOLUME) != 0 || vol->integrity_meta_oid != 0 ||
	    vol->num_snapshots != 0) {
		return EROFS;
	}
	error = apfs_spaceman_open(vol->nx, &sm);
	if (error) {
		apfs_log("fs: no space manager for a transaction: %d\n", error);
		return error;
	}
	txn = apfs_txn_begin(vol->nx, &sm);
	if (txn == NULL) {
		apfs_spaceman_close(&sm);
		return EIO;
	}
	error = fs_open(&fc, vol, txn);
	if (error) {
		apfs_log("fs: cannot open the volume's trees: %d\n", error);
	}
	if (error == 0) {
		error = op(&fc, arg);
		if (error) {
			apfs_log("fs: operation refused: %d by %s\n", error, who_name());
		}
	}
	if (error == 0) {
		error = fs_commit(&fc, when);
		if (error) {
			apfs_log("fs: commit failed: %d; disabling writes to protect filesystem\n", error);
			vol->nx->dev.write = NULL;
		}
	}
	fs_close(&fc);
	apfs_txn_abort(txn);
	apfs_spaceman_close(&sm);
	return error;
}

struct create_arg {
	uint64_t parent;
	char     name[FS_NAME_MAX];
	uint16_t mode;
	uint32_t owner;
	uint32_t group;
	uint64_t *new_id;
};

static int
op_create(struct fs_ctx *fc, void *arg)
{
	struct create_arg *a = arg;

	return fs_add_file(fc, a->parent, a->name, a->mode, a->owner, a->group, false,
	    apfs_txn_xid(fc->txn), a->new_id);
}

int
apfs_create_file(struct apfs_vol *vol, uint64_t parent, const char *name, uint16_t mode,
    uint32_t owner, uint32_t group, uint64_t when, uint64_t *new_id)
{
	struct create_arg a;

	memset(&a, 0, sizeof(a));
	a.parent = parent;
	a.mode = mode;
	a.owner = owner;
	a.group = group;
	a.new_id = new_id;
	if (strlen(name) >= sizeof(a.name)) {
		return ENAMETOOLONG;
	}
	memcpy(a.name, name, strlen(name) + 1);
	return fs_run(vol, op_create, &a, when);
}



/* A hole: an extent record that names no block.  A sparse file's extents have to run from
 * zero without a break -- apfsck reads a gap as lost data -- so the holes are written out. */
static int
fs_emit_hole(struct fs_ctx *fc, uint64_t sid, uint64_t off, uint64_t length)
{
	uint8_t key[16], val[24];

	if (length == 0) {
		return 0;
	}
	fs_key(key, sid, APFS_TYPE_FILE_EXTENT);
	f_put64(key + 8, off);
	f_put64(val, length);
	f_put64(val + 8, 0);
	f_put64(val + 16, 0);
	return btp_update(&fc->fs, BTP_INSERT, key, 16, val, 24);
}

/* Every byte from zero to the end of the file belongs to an extent: fill the gaps with holes
 * up to the file's size, rounded to a block. */
static int
fs_holes_fill(struct fs_ctx *fc, uint64_t sid, uint64_t size)
{
	uint32_t bs = apfs_txn_block_size(fc->txn);
	uint64_t at = 0, need = ((size + bs - 1) / bs) * bs;
	struct apfs_extent ex;
	int error;

	for (;;) {
		error = apfs_extent_next(fc->vol, sid, at, &ex);
		if (error == ENOENT) {
			break;
		}
		if (error) {
			return error;
		}
		if (ex.logical > at) {
			error = fs_emit_hole(fc, sid, at, ex.logical - at);
			if (error) {
				return error;
			}
		}
		at = ex.logical + ex.length;
	}
	if (at < need) {
		error = fs_emit_hole(fc, sid, at, need - at);
		if (error) {
			return error;
		}
	}
	return 0;
}

/* How much of the file's logical space the extent records cover, holes included.  The inode's
 * data stream says this, and apfsck wants it to match the records. */
static int
fs_extent_total(struct fs_ctx *fc, uint64_t sid, uint64_t *total)
{
	struct apfs_extent ex;
	uint64_t at = 0;

	*total = 0;
	for (;;) {
		int error = apfs_extent_next(fc->vol, sid, at, &ex);

		if (error == ENOENT) {
			return 0;
		}
		if (error) {
			return error;
		}
		*total += ex.length;
		at = ex.logical + ex.length;
	}
}

/* The holes' part of that: what the inode's sparse-bytes xfield says. */
static int
fs_extent_sparse(struct fs_ctx *fc, uint64_t sid, uint64_t *sparse)
{
	struct apfs_extent ex;
	uint64_t at = 0;

	*sparse = 0;
	for (;;) {
		int error = apfs_extent_next(fc->vol, sid, at, &ex);

		if (error == ENOENT) {
			return 0;
		}
		if (error) {
			return error;
		}
		if (ex.phys == 0) {
			*sparse += ex.length;
		}
		at = ex.logical + ex.length;
	}
}

/* The extents of one file, as the write sees them: what the tree holds plus what this
 * transaction has put there.  A write has to know both or it makes a record overlapping one
 * it just made. */
static void
fs_ext_list_free(struct fs_ext_list *l)
{
	if (l->e != NULL) {
		apfs_free(l->e, (size_t)l->cap * sizeof(*l->e));
	}
	l->e = NULL;
	l->n = l->cap = 0;
}

static int
fs_ext_list_load(struct fs_ctx *fc, uint64_t sid, struct fs_ext_list *l)
{
	struct apfs_extent ex;
	uint64_t at = 0;
	uint32_t cap = 16;
	int error;

	memset(l, 0, sizeof(*l));
	l->e = apfs_alloc((size_t)cap * sizeof(*l->e));
	if (l->e == NULL) {
		return ENOMEM;
	}
	l->cap = cap;
	for (;;) {
		error = apfs_extent_next(fc->vol, sid, at, &ex);
		if (error == ENOENT) {
			return 0;
		}
		if (error) {
			fs_ext_list_free(l);
			return error;
		}
		if (l->n == l->cap) {
			struct apfs_extent *bigger;
			uint32_t ncap = l->cap * 2;

			bigger = apfs_alloc((size_t)ncap * sizeof(*bigger));
			if (bigger == NULL) {
				fs_ext_list_free(l);
				return ENOMEM;
			}
			memcpy(bigger, l->e, (size_t)l->n * sizeof(*bigger));
			apfs_free(l->e, (size_t)l->cap * sizeof(*l->e));
			l->e = bigger;
			l->cap = ncap;
		}
		l->e[l->n++] = ex;
		at = ex.logical + ex.length;
	}
}

static uint32_t
fs_ext_list_find(const struct fs_ext_list *l, uint64_t off)
{
	uint32_t i;

	for (i = 0; i < l->n; i++) {
		if (off < l->e[i].logical) {
			return i;
		}
		if (off < l->e[i].logical + l->e[i].length) {
			return i;
		}
	}
	return l->n;
}

/* Insert in logical order.  Callers only insert where nothing covers the range. */
static int
fs_ext_list_add(struct fs_ext_list *l, uint64_t logical, uint64_t length, uint64_t phys)
{
	uint32_t at = 0, i;

	if (l->n == l->cap) {
		uint32_t ncap = l->cap != 0 ? l->cap * 2 : 16;
		struct apfs_extent *bigger = apfs_alloc((size_t)ncap * sizeof(*bigger));

		if (bigger == NULL) {
			return ENOMEM;
		}
		if (l->e != NULL) {
			memcpy(bigger, l->e, (size_t)l->n * sizeof(*bigger));
			apfs_free(l->e, (size_t)l->cap * sizeof(*l->e));
		}
		l->e = bigger;
		l->cap = ncap;
	}
	while (at < l->n && l->e[at].logical < logical) {
		at++;
	}
	for (i = l->n; i > at; i--) {
		l->e[i] = l->e[i - 1];
	}
	l->e[at].logical = logical;
	l->e[at].length = length;
	l->e[at].phys = phys;
	l->n++;
	return 0;
}

static void
fs_ext_list_del(struct fs_ext_list *l, uint32_t i)
{
	memmove(&l->e[i], &l->e[i + 1], (size_t)(l->n - i - 1) * sizeof(*l->e));
	l->n--;
}

/* A record for one entry of the list. */
static int
fs_ext_emit(struct fs_ctx *fc, uint64_t sid, const struct apfs_extent *ex)
{
	if (ex->phys == 0) {
		return fs_emit_hole(fc, sid, ex->logical, ex->length);
	}
	return fs_emit_run(fc, sid, ex->logical, ex->phys,
	    (uint32_t)(ex->length / apfs_txn_block_size(fc->txn)));
}

static int
fs_ext_drop_record(struct fs_ctx *fc, uint64_t sid, const struct apfs_extent *ex)
{
	uint8_t key[16];

	fs_key(key, sid, APFS_TYPE_FILE_EXTENT);
	f_put64(key + 8, ex->logical);
	return btp_update(&fc->fs, BTP_DELETE, key, 16, NULL, 0);
}

/* Write out the gaps of the list up to `size` (a block boundary) and say how much of the file
 * the records cover and how much of that is holes. */
static uint32_t
fs_ext_list_refs(const struct fs_ext_list *l)
{
	uint32_t i, n = 0;

	for (i = 0; i < l->n; i++) {
		if (l->e[i].phys != 0) {
			n++;
		}
	}
	return n;
}

static int
fs_ext_list_finish(struct fs_ctx *fc, uint64_t sid, struct fs_ext_list *l, uint64_t size,
    uint64_t *total, uint64_t *sparse)
{
	uint32_t bs = apfs_txn_block_size(fc->txn);
	uint64_t at = 0, need = ((size + bs - 1) / bs) * bs;
	uint32_t i;

	*total = 0;
	*sparse = 0;
	for (i = 0; i < l->n; i++) {
		uint64_t logical = l->e[i].logical;
		uint64_t length = l->e[i].length;

		if (logical > at) {
			int error = fs_emit_hole(fc, sid, at, logical - at);

			if (error) {
				return error;
			}
			*total += logical - at;
			*sparse += logical - at;
		}
		*total += length;
		if (l->e[i].phys == 0) {
			*sparse += length;
		}
		at = logical + length;
	}
	if (at < need) {
		int error = fs_emit_hole(fc, sid, at, need - at);

		if (error) {
			return error;
		}
		*total += need - at;
		*sparse += need - at;
	}
	return 0;
}

struct write_arg {
	uint64_t       obj_id;
	uint64_t       off;
	const uint8_t *buf;
	size_t         len;
};

/*
 * A write of file contents at any offset.  A block that already belongs to the file is read,
 * patched and staged; a block in a hole (or past the last extent) is allocated, zeroed and
 * takes its place among the extent records, with the holes around it kept as records of their
 * own -- APFS wants a stream's extents to run from zero without a break.
 */
static int
op_write(struct fs_ctx *fc, void *arg)
{
	struct write_arg *a = arg;
	uint32_t bs = apfs_txn_block_size(fc->txn);
	uint32_t k;
	struct apfs_inode ino;
	struct fs_ext_list l;
	uint8_t *ival = NULL, *blk = NULL, *ds;
	uint16_t ivlen, dslen;
	uint64_t size, alloced, sparse, sid, end, at;
	const uint8_t *cur;
	uint8_t key[16], val[8];
	int error;

	error = apfs_inode_get(fc->vol, a->obj_id, &ino);
	if (error) {
		return error;
	}
	if ((ino.mode & APFS_S_IFMT) != APFS_S_IFREG) {
		return EINVAL;
	}
	if (a->len == 0) {
		return 0;
	}
	ival = apfs_alloc(bs);
	blk = apfs_alloc(bs);
	if (ival == NULL || blk == NULL) {
		error = ENOMEM;
		goto out;
	}
	error = apfs_inode_value_get(fc->vol, a->obj_id, ival, bs, &ivlen);
	if (error) {
		goto out;
	}
	ds = inode_xfield(ival, ivlen, APFS_INO_EXT_TYPE_DSTREAM, &dslen);
	if (ds == NULL) {
		uint8_t zero[40];

		memset(zero, 0, sizeof(zero));
		error = inode_xfield_set(ival, &ivlen, APFS_INO_EXT_TYPE_DSTREAM,
		    APFS_XF_SYSTEM_FIELD, zero, sizeof(zero));
		if (error) {
			goto out;
		}
		ds = inode_xfield(ival, ivlen, APFS_INO_EXT_TYPE_DSTREAM, &dslen);
	}
	if (ds == NULL || dslen < 32) {
		error = EIO;
		goto out;
	}
	size = f_le64(ds);
	alloced = f_le64(ds + 8);
	sid = ino.private_id;
	end = a->off + a->len;
	error = fs_ext_list_load(fc, sid, &l);
	if (error) {
		goto out;
	}

	for (at = a->off; at < end;) {
		uint64_t boff = at - at % bs;
		size_t in_block = (size_t)(at - boff);
		size_t take = bs - in_block;
		uint32_t i = fs_ext_list_find(&l, boff);
		uint64_t phys;

		if (take > end - at) {
			take = (size_t)(end - at);
		}
		if (i < l.n && l.e[i].phys != 0) {
			phys = l.e[i].phys + (boff - l.e[i].logical) / bs;
			error = apfs_block_read_raw(fc->vol->nx, phys, &cur);
			if (error) {
				goto out_list;
			}
			memcpy(blk, cur, bs);
		} else {
			struct apfs_extent hole;

			memset(blk, 0, bs);
			error = apfs_txn_alloc_block(fc->txn, &phys);
			if (error) {
				goto out_list;
			}
			if (i < l.n) {
				/* the block lies in a hole that has a record: split it */
				hole = l.e[i];
				error = fs_ext_drop_record(fc, sid, &hole);
				if (error != 0 && error != ENOENT) {
					goto out_list;
				}
				fs_ext_list_del(&l, i);
				if (boff > hole.logical) {
					error = fs_ext_list_add(&l, hole.logical,
					    boff - hole.logical, 0);
					if (error) {
						goto out_list;
					}
					error = fs_emit_hole(fc, sid, hole.logical,
					    boff - hole.logical);
					if (error) {
						goto out_list;
					}
				}
				if (boff + bs < hole.logical + hole.length) {
					uint64_t start = boff + bs;

					error = fs_ext_list_add(&l, start,
					    hole.logical + hole.length - start, 0);
					if (error) {
						goto out_list;
					}
					error = fs_emit_hole(fc, sid, start,
					    hole.logical + hole.length - start);
					if (error) {
						goto out_list;
					}
				}
			} else {
				/* past the last extent: a hole before it has to be filled */
				uint64_t prev = i > 0 ?
				    l.e[i - 1].logical + l.e[i - 1].length : 0;

				if (prev < boff) {
					error = fs_ext_list_add(&l, prev, boff - prev, 0);
					if (error) {
						goto out_list;
					}
					error = fs_emit_hole(fc, sid, prev, boff - prev);
					if (error) {
						goto out_list;
					}
				}
			}
			if (i == l.n && i > 0 && l.e[i - 1].phys != 0 &&
			    l.e[i - 1].logical + l.e[i - 1].length == boff &&
			    l.e[i - 1].phys + l.e[i - 1].length / bs == phys) {
				/* one run of blocks, one extent record -- and it goes in after the loop */
				l.e[i - 1].length += bs;
			} else {
				error = fs_ext_list_add(&l, boff, bs, phys);
				if (error) {
					goto out_list;
				}
			}
			fc->data_delta++;
		}
		memcpy(blk + in_block, (const uint8_t *)a->buf + (at - a->off), take);
		error = apfs_txn_stage_block(fc->txn, phys, blk);
		if (error) {
			goto out_list;
		}
		at += take;
	}

	/*
	 * The extent records once, after the whole write, and not once per block: a record per
	 * 4 KB block was a search of the volume's tree and a copy-on-write of its path, so a
	 * write of a few megabytes staged thousands of blocks -- and every write held the
	 * writer's mutex for as long as putting them out took (p393: 0.3 to 1.1 s a time;
	 * p394: SpringBoard's pwrite of the wallpaper's cache, 4 s, and everything else waited).
	 *
	 * Only the extents the write touched: p397 wrote all of them out every time, so a file
	 * grown in small appends re-recorded its whole length on every append (and the boot
	 * never got as far as the first frame).
	 */
	for (k = 0; k < l.n; k++) {
		if (l.e[k].phys != 0 && l.e[k].logical + l.e[k].length > a->off &&
		    l.e[k].logical < end) {
			error = fs_emit_run(fc, sid, l.e[k].logical, l.e[k].phys,
			    (uint32_t)(l.e[k].length / bs));
			if (error) {
				goto out_list;
			}
		}
	}

	if (end > size) {
		size = end;
	}
	error = fs_ext_list_finish(fc, sid, &l, size, &alloced, &sparse);
	if (error) {
		goto out_list;
	}
	f_put64(ds, size);
	f_put64(ds + 8, alloced);
	error = inode_set_sparse(ival, &ivlen, sparse);
	if (error) {
		goto out_list;
	}
	f_put64(ival + offsetof(struct apfs_j_inode_val, mod_time),
	    apfs_txn_xid(fc->txn));
	f_put64(ival + offsetof(struct apfs_j_inode_val, change_time),
	    apfs_txn_xid(fc->txn));
	fs_key(key, a->obj_id, APFS_TYPE_INODE);
	error = btp_update(&fc->fs, BTP_REPLACE, key, 8, ival, ivlen);
	if (error) {
		goto out_list;
	}
	/* and the record that says the stream has extents at all */
	if (size > 0) {
		fs_key(key, sid, APFS_TYPE_DSTREAM_ID);
		f_put32(val, 1);
		error = btp_update(&fc->fs, BTP_REPLACE, key, 8, val, 4);
		if (error == ENOENT) {
			error = btp_update(&fc->fs, BTP_INSERT, key, 8, val, 4);
		}
	}
out_list:
	fs_ext_list_free(&l);
out:
	if (ival != NULL) {
		apfs_free(ival, bs);
	}
	if (blk != NULL) {
		apfs_free(blk, bs);
	}
	return error;
}

int
apfs_write_data(struct apfs_vol *vol, uint64_t obj_id, uint64_t off, const void *buf,
    size_t len, uint64_t when)
{
	struct write_arg a;

	a.obj_id = obj_id;
	a.off = off;
	a.buf = buf;
	a.len = len;
	return fs_run(vol, op_write, &a, when);
}

int
apfs_append_file(struct apfs_vol *vol, uint64_t obj_id, const void *buf, size_t len,
    uint64_t when)
{
	struct write_arg a;
	struct apfs_inode ino;
	int error;

	/*
	 * The kernel never appends by itself: VNOP_WRITE with O_APPEND hands apfs_write_data the
	 * offset the file ends at (apfs_vnops.c), and that is the only write path.  This does the
	 * same, so what is tested here is what the phone runs -- an append into a file whose end
	 * is a hole included (that used to be refused with EIO).
	 */
	error = apfs_inode_get(vol, obj_id, &ino);
	if (error) {
		return error;
	}
	a.obj_id = obj_id;
	a.off = ino.size;
	a.buf = buf;
	a.len = len;
	return fs_run(vol, op_write, &a, when);
}

struct unlink_arg {
	uint64_t parent;
	char     name[FS_NAME_MAX];
	bool     keep;          /* the last name of a file that is still open: the file stays */
};

static int
op_unlink(struct fs_ctx *fc, void *arg)
{
	struct unlink_arg *a = arg;

	return fs_remove_file(fc, a->parent, a->name, false, a->keep, apfs_txn_xid(fc->txn));
}

static int
op_mkdir(struct fs_ctx *fc, void *arg)
{
	struct create_arg *a = arg;

	return fs_add_file(fc, a->parent, a->name, a->mode, a->owner, a->group, true,
	    apfs_txn_xid(fc->txn), a->new_id);
}

int
apfs_mkdir(struct apfs_vol *vol, uint64_t parent, const char *name, uint16_t mode,
    uint32_t owner, uint32_t group, uint64_t when, uint64_t *new_id)
{
	struct create_arg a;

	memset(&a, 0, sizeof(a));
	a.parent = parent;
	a.mode = mode;
	a.owner = owner;
	a.group = group;
	a.new_id = new_id;
	if (strlen(name) >= sizeof(a.name)) {
		return ENAMETOOLONG;
	}
	memcpy(a.name, name, strlen(name) + 1);
	return fs_run(vol, op_mkdir, &a, when);
}

static int
op_rmdir(struct fs_ctx *fc, void *arg)
{
	struct unlink_arg *a = arg;

	return fs_remove_file(fc, a->parent, a->name, true, false, apfs_txn_xid(fc->txn));
}

int
apfs_rmdir(struct apfs_vol *vol, uint64_t parent, const char *name, uint64_t when)
{
	struct unlink_arg a;

	memset(&a, 0, sizeof(a));
	a.parent = parent;
	if (strlen(name) >= sizeof(a.name)) {
		return ENAMETOOLONG;
	}
	memcpy(a.name, name, strlen(name) + 1);
	return fs_run(vol, op_rmdir, &a, when);
}

int
apfs_unlink(struct apfs_vol *vol, uint64_t parent, const char *name, uint64_t when)
{
	struct unlink_arg a;

	memset(&a, 0, sizeof(a));
	a.parent = parent;
	if (strlen(name) >= sizeof(a.name)) {
		return ENAMETOOLONG;
	}
	memcpy(a.name, name, strlen(name) + 1);
	a.keep = false;
	return fs_run(vol, op_unlink, &a, when);
}

/*
 * The last name of a file that is still open: the name goes and the link count falls to
 * zero, but the inode, its extents and its bytes stay until the last close (VNOP_INACTIVE
 * calls apfs_delete_inode).  A reader in the file is still reading them.
 */
int
apfs_unlink_keep(struct apfs_vol *vol, uint64_t parent, const char *name, uint64_t when)
{
	struct unlink_arg a;

	memset(&a, 0, sizeof(a));
	a.parent = parent;
	if (strlen(name) >= sizeof(a.name)) {
		return ENAMETOOLONG;
	}
	memcpy(a.name, name, strlen(name) + 1);
	a.keep = true;
	return fs_run(vol, op_unlink, &a, when);
}

struct orphan_arg {
	uint64_t obj_id;
};

static int
op_orphan_delete(struct fs_ctx *fc, void *arg)
{
	struct orphan_arg *a = arg;
	struct apfs_inode ino;
	int error;

	error = apfs_inode_get(fc->vol, a->obj_id, &ino);
	if (error) {
		return error == ENOENT ? 0 : error;
	}
	/*
	 * The directory counted the inode out when the name went (nchildren fell to zero).
	 * If a name is back -- cfprefsd's lock file, a database made again after a delete --
	 * the inode is not an orphan, and taking it away would fail the next write with
	 * ENOENT, which SQLite reports as a disk I/O error.
	 */
	if ((ino.mode & APFS_S_IFMT) != APFS_S_IFDIR && ino.nchildren > 0) {
		apfs_log("fs: orphan delete of inode %llu: it still has %d names\n",
		    (unsigned long long)a->obj_id, (int)ino.nchildren);
		return 0;
	}
	/* no name, and the directory counted it out when the name went */
	return fs_delete_inode(fc, a->obj_id, &ino, false, 0, false, apfs_txn_xid(fc->txn));
}

/* A file whose last name went while it was open: nothing names it, and this is its end. */
int
apfs_delete_inode(struct apfs_vol *vol, uint64_t obj_id, uint64_t when)
{
	struct orphan_arg a;

	a.obj_id = obj_id;
	return fs_run(vol, op_orphan_delete, &a, when);
}

int
apfs_rename(struct apfs_vol *vol, uint64_t parent, const char *name, uint64_t new_parent,
    const char *new_name, uint64_t when)
{
	struct rename_arg a;

	memset(&a, 0, sizeof(a));
	a.parent = parent;
	a.new_parent = new_parent;
	if (strlen(name) >= sizeof(a.name) || strlen(new_name) >= sizeof(a.new_name)) {
		return ENAMETOOLONG;
	}
	memcpy(a.name, name, strlen(name) + 1);
	memcpy(a.new_name, new_name, strlen(new_name) + 1);
	apfs_log("fs: rename %s in %llu -> %s in %llu by %s\n", name,
	    (unsigned long long)parent, new_name, (unsigned long long)new_parent, who_name());
	return fs_run(vol, op_rename, &a, when);
}

int
apfs_link(struct apfs_vol *vol, uint64_t parent, const char *name, uint64_t obj_id,
    uint64_t when)
{
	struct link_arg a;

	memset(&a, 0, sizeof(a));
	a.parent = parent;
	a.obj_id = obj_id;
	if (strlen(name) >= sizeof(a.name)) {
		return ENAMETOOLONG;
	}
	memcpy(a.name, name, strlen(name) + 1);
	return fs_run(vol, op_link, &a, when);
}

int
apfs_truncate(struct apfs_vol *vol, uint64_t obj_id, uint64_t size, uint64_t when)
{
	struct truncate_arg a;

	a.obj_id = obj_id;
	a.size = size;
	return fs_run(vol, op_truncate, &a, when);
}

int
apfs_setxattr(struct apfs_vol *vol, uint64_t obj_id, const char *name, const void *data,
    size_t len, uint16_t flags, uint64_t when)
{
	struct xattr_arg a;

	memset(&a, 0, sizeof(a));
	a.obj_id = obj_id;
	a.data = data;
	a.len = len;
	a.flags = (uint16_t)(flags & ~APFS_XATTR_DATA_STREAM);
	if (strlen(name) >= sizeof(a.name)) {
		return ENAMETOOLONG;
	}
	memcpy(a.name, name, strlen(name) + 1);
	return fs_run(vol, op_xattr, &a, when);
}

int
apfs_removexattr(struct apfs_vol *vol, uint64_t obj_id, const char *name, uint64_t when)
{
	struct xattr_arg a;

	memset(&a, 0, sizeof(a));
	a.obj_id = obj_id;
	a.remove = true;
	if (strlen(name) >= sizeof(a.name)) {
		return ENAMETOOLONG;
	}
	memcpy(a.name, name, strlen(name) + 1);
	return fs_run(vol, op_xattr, &a, when);
}

struct setattr_arg {
	uint64_t obj_id;
	struct apfs_setattr sa;
};

static int
op_setattr(struct fs_ctx *fc, void *arg)
{
	struct setattr_arg *a = arg;
	uint8_t *ival = NULL, key[8];
	uint16_t ivlen;
	uint32_t bs = apfs_txn_block_size(fc->txn);
	uint64_t when = apfs_txn_xid(fc->txn);
	int error;

	ival = apfs_alloc(bs);
	if (ival == NULL) {
		return ENOMEM;
	}
	error = apfs_inode_value_get(fc->vol, a->obj_id, ival, bs, &ivlen);
	if (error) {
		apfs_log("setattr: value of inode %llu: %d\n", (unsigned long long)a->obj_id, error);
	}
	if (error == 0) {
		if (a->sa.mode_set) {
			f_put16(ival + offsetof(struct apfs_j_inode_val, mode), a->sa.mode);
		}
		if (a->sa.owner_set) {
			f_put32(ival + offsetof(struct apfs_j_inode_val, owner), a->sa.owner);
		}
		if (a->sa.group_set) {
			f_put32(ival + offsetof(struct apfs_j_inode_val, group), a->sa.group);
		}
		if (a->sa.flags_set) {
			f_put32(ival + offsetof(struct apfs_j_inode_val, bsd_flags),
			    a->sa.bsd_flags);
		}
		if (a->sa.atime_set) {
			f_put64(ival + offsetof(struct apfs_j_inode_val, access_time),
			    a->sa.atime);
		}
		if (a->sa.mtime_set) {
			f_put64(ival + offsetof(struct apfs_j_inode_val, mod_time), a->sa.mtime);
		}
		f_put64(ival + offsetof(struct apfs_j_inode_val, change_time), when);
		fs_key(key, a->obj_id, APFS_TYPE_INODE);
		error = btp_update(&fc->fs, BTP_REPLACE, key, 8, ival, ivlen);
		if (error) {
			apfs_log("setattr: replace of inode %llu (%u bytes): %d\n",
			    (unsigned long long)a->obj_id, (unsigned)ivlen, error);
		}
	}
	apfs_free(ival, bs);
	return error;
}

int
apfs_inode_setattr(struct apfs_vol *vol, uint64_t obj_id, const struct apfs_setattr *sa,
    uint64_t when)
{
	struct setattr_arg a;

	a.obj_id = obj_id;
	a.sa = *sa;
	return fs_run(vol, op_setattr, &a, when);
}

#ifndef KERNEL
/*
 * A check of the extended-field re-packing, for the host tests: rebuild an inode's record
 * from the values it already has, and say whether the bytes came out the same.  When they
 * did, the rebuilt record is put back through a transaction, so the image the checkers see
 * has really been through the code rename and truncate use.
 */
struct repack_arg {
	uint64_t obj_id;
	bool     same;
};

static int
op_repack(struct fs_ctx *fc, void *arg)
{
	struct repack_arg *a = arg;
	uint8_t *src = NULL, *dst = NULL, *name, *ds;
	char nm[FS_NAME_MAX];
	uint16_t slen, dlen, nlen, dslen;
	uint8_t key[8];
	uint32_t bs = apfs_txn_block_size(fc->txn);
	int error;

	src = apfs_alloc(bs);
	dst = apfs_alloc(bs);
	if (src == NULL || dst == NULL) {
		error = ENOMEM;
		goto out;
	}
	error = apfs_inode_value_get(fc->vol, a->obj_id, src, bs, &slen);
	if (error) {
		goto out;
	}
	memcpy(dst, src, slen);
	dlen = slen;
	name = inode_xfield(src, slen, APFS_INO_EXT_TYPE_NAME, &nlen);
	if (name == NULL || nlen < 2 || nlen > sizeof(nm)) {
		error = EIO;
		goto out;
	}
	memcpy(nm, name, nlen);                 /* it carries its zero: a C string */
	error = inode_set_name(dst, &dlen, nm, nlen - 1);
	if (error) {
		goto out;
	}
	ds = inode_xfield(src, slen, APFS_INO_EXT_TYPE_DSTREAM, &dslen);
	if (ds != NULL && dslen >= 16) {
		error = inode_set_dstream(dst, &dlen, f_le64(ds), f_le64(ds + 8));
		if (error) {
			goto out;
		}
	}
	a->same = (slen == dlen) && memcmp(src, dst, slen) == 0;
	if (a->same) {
		fs_key(key, a->obj_id, APFS_TYPE_INODE);
		error = btp_update(&fc->fs, BTP_REPLACE, key, 8, dst, dlen);
	}
out:
	if (src != NULL) {
		apfs_free(src, bs);
	}
	if (dst != NULL) {
		apfs_free(dst, bs);
	}
	return error;
}

int
apfs_inode_repack_check(struct apfs_vol *vol, uint64_t obj_id, bool *same, uint64_t when)
{
	struct repack_arg a;
	int error;

	a.obj_id = obj_id;
	a.same = false;
	error = fs_run(vol, op_repack, &a, when);
	if (same != NULL) {
		*same = a.same;
	}
	return error;
}
#endif /* !KERNEL */
