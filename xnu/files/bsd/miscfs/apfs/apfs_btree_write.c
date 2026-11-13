/*
 * Writing into APFS: a B-tree with a change made to it.
 *
 * The trees of a volume -- its file-system tree, its object map, the container's -- are
 * copy-on-write: no node that a reader may still be looking at is changed in place.  A change
 * copies the leaf that holds the record, then the node above it, and so on to the root; a node
 * that has no room for the record is split in two and the split goes up the same way.  What
 * this file does is that, for one record at a time, bottom up:
 *
 *   - the leaf: the record inserted, deleted or replaced, entries kept in the order the
 *     reader sorts them by (a comparator from the caller)
 *   - every node above it: the entry for the child that changed gets the child's new
 *     identifier and, when its first key moved, its new key; a split adds an entry beside it
 *   - a node that no longer fits is cut in two; a root that no longer fits becomes two
 *     children and a new root above them
 *   - every old block is given back to the space manager, every new one taken from it
 *
 * What it does not do: merge underfull nodes, and shrink a tree by a level.  A node is allowed
 * to be emptier than APFS would keep it, which costs space and not correctness; the readers
 * here and apfsck both accept it.  Nodes of a sealed tree (the iOS system volume) are not
 * written at all -- that tree is not the one this port opens for writing.
 *
 * New nodes of a virtual tree (the file-system tree) are new object identifiers, taken from
 * the volume's next object identifier, and every one of them has to be entered in the
 * volume's object map: that is the caller's business, and btp_change_at() is what it is told
 * with.  Descending such a tree needs the map too, which is why the caller hands over a
 * resolver.  A physical tree (the object maps themselves) names its nodes by their block
 * address and needs neither.
 */

#include <stddef.h>

#include "apfs_txn_priv.h"

/* A record held in memory while a node is being changed. */
struct btp_entry {
	uint8_t *k;
	uint16_t kl;
	uint8_t *v;
	uint16_t vl;
};

struct btp_node {
	uint16_t flags;
	uint16_t level;
	uint32_t nkeys;
	struct btp_entry *e;
};


/* ------------------------------------------------------------------ little-endian */

static inline uint32_t
b_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static inline uint64_t
b_le64(const uint8_t *p)
{
	return (uint64_t)b_le32(p) | (uint64_t)b_le32(p + 4) << 32;
}

static inline void
b_put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

static inline void
b_put64(uint8_t *p, uint64_t v)
{
	b_put32(p, (uint32_t)v);
	b_put32(p + 4, (uint32_t)(v >> 32));
}

/* ---------------------------------------------------------------- a node in memory */

static void
btp_node_free(struct btp_node *n)
{
	uint32_t i;

	for (i = 0; i < n->nkeys; i++) {
		apfs_free(n->e[i].k, n->e[i].kl != 0 ? n->e[i].kl : 1);
		apfs_free(n->e[i].v, n->e[i].vl != 0 ? n->e[i].vl : 1);
	}
	if (n->e != NULL) {
		apfs_free(n->e, (n->nkeys != 0 ? n->nkeys : 1) * sizeof(*n->e));
	}
	memset(n, 0, sizeof(*n));
}

static int
btp_entry_grow(struct btp_node *n, uint32_t need)
{
	struct btp_entry *e;

	if (need <= n->nkeys && n->e != NULL) {
		return 0;
	}
	e = apfs_alloc(need * sizeof(*e));
	if (e == NULL) {
		return ENOMEM;
	}
	if (n->e != NULL) {
		memcpy(e, n->e, n->nkeys * sizeof(*e));
		apfs_free(n->e, (n->nkeys != 0 ? n->nkeys : 1) * sizeof(*n->e));
	}
	n->e = e;
	return 0;
}

static int
btp_node_load(struct btp_tree *t, uint64_t addr, struct btp_node *n)
{
	uint8_t *blk = NULL;
	uint32_t bs = t->bs, i, nkeys, toc_off, toc_len, toc, key_base, val_end;
	uint16_t flags, level;
	int error;

	memset(n, 0, sizeof(*n));
	error = apfs_txn_read_block(t->txn, addr, &blk);
	if (error) {
		return error;
	}
	flags = (uint16_t)(blk[32] | (uint16_t)blk[33] << 8);
	level = (uint16_t)(blk[34] | (uint16_t)blk[35] << 8);
	nkeys = b_le32(blk + 36);
	toc_off = (uint32_t)(blk[40] | (uint16_t)blk[41] << 8);
	toc_len = (uint32_t)(blk[42] | (uint16_t)blk[43] << 8);
	toc = 56 + toc_off;
	key_base = toc + toc_len;
	val_end = (flags & 0x0001) ? bs - 40 : bs;
	if (toc < 56 || key_base > val_end || nkeys > (bs - 56) / 4) {
		apfs_log("inode insert: block %llu is not a node (flags %#x level %u keys %u)\n",
		    (unsigned long long)addr, flags, level, nkeys);
		apfs_free(blk, bs);
		return EIO;
	}
	n->flags = flags;
	n->level = level;
	if (btp_entry_grow(n, nkeys != 0 ? nkeys : 1) != 0) {
		apfs_free(blk, bs);
		return ENOMEM;
	}
	n->nkeys = nkeys;
	for (i = 0; i < nkeys; i++) {
		uint32_t koff, klen, voff, vlen;

		if (flags & 0x0004) {
			if (toc + 4 * (size_t)(i + 1) > key_base) {
				goto bad;
			}
			koff = (uint32_t)(blk[toc + 4 * i] | (uint16_t)blk[toc + 4 * i + 1] << 8);
			voff = (uint32_t)(blk[toc + 4 * i + 2] | (uint16_t)blk[toc + 4 * i + 3] << 8);
			klen = t->key_size;
			vlen = level == 0 ? t->val_size : 8;
		} else {
			if (toc + 8 * (size_t)(i + 1) > key_base) {
				goto bad;
			}
			koff = (uint32_t)(blk[toc + 8 * i] | (uint16_t)blk[toc + 8 * i + 1] << 8);
			klen = (uint32_t)(blk[toc + 8 * i + 2] | (uint16_t)blk[toc + 8 * i + 3] << 8);
			voff = (uint32_t)(blk[toc + 8 * i + 4] | (uint16_t)blk[toc + 8 * i + 5] << 8);
			vlen = (uint32_t)(blk[toc + 8 * i + 6] | (uint16_t)blk[toc + 8 * i + 7] << 8);
		}
		/*
		 * The value's offset in the table is cumulative -- it counts its own length and
		 * all the values before it, because the values are laid out from the end of the
		 * block backwards.  So the record must end at or before the end and begin at or
		 * after the block's start.
		 */
		if (key_base + koff + klen > val_end || voff > val_end || voff < vlen) {
			goto bad;
		}
		if (!t->fixed) {
			if ((level == 0 && (klen < 8 || vlen == 0)) ||
			    (level > 0 && i > 0 && klen < 8) ||
			    (level > 0 && vlen != 8)) {
				goto bad;
			}
		}
		n->e[i].k = apfs_alloc(klen != 0 ? klen : 1);
		n->e[i].v = apfs_alloc(vlen != 0 ? vlen : 1);
		if (n->e[i].k == NULL || n->e[i].v == NULL) {
			apfs_free(blk, bs);
			return ENOMEM;
		}
		memcpy(n->e[i].k, blk + key_base + koff, klen);
		memcpy(n->e[i].v, blk + val_end - voff, vlen);
		n->e[i].kl = (uint16_t)klen;
		n->e[i].vl = (uint16_t)vlen;
	}
	apfs_free(blk, bs);
	return 0;
bad:
	btp_node_free(n);
	apfs_free(blk, bs);
	return EIO;
}

static uint32_t
btp_toc_len(struct btp_tree *t, uint32_t nkeys, bool leaf)
{
	if (t->fixed) {
		uint32_t vs = leaf ? t->val_size : 8;

		return ((t->bs - 56) / (t->key_size + vs + 4)) * 4;
	}
	if (nkeys < 8) {
		nkeys = 8;
	}
	return ((nkeys + 7) / 8) * 64;
}

static uint32_t
btp_node_bytes(struct btp_tree *t, struct btp_node *n, bool root)
{
	uint32_t total = 56 + btp_toc_len(t, n->nkeys, n->level == 0), i;

	for (i = 0; i < n->nkeys; i++) {
		total += n->e[i].kl + n->e[i].vl;
	}
	return total + (root ? 40 : 0);
}

static int
btp_node_pack(struct btp_tree *t, struct btp_node *n, bool root, apfs_oid_t oid, uint8_t *blk)
{
	uint32_t bs = t->bs, toc_len = btp_toc_len(t, n->nkeys, n->level == 0);
	uint32_t key_base = 56 + toc_len, val_end = root ? bs - 40 : bs;
	uint32_t koff = 0, voff = 0, i, free_bytes;
	uint16_t flags = (uint16_t)((root ? 0x0001 : 0) | (n->level == 0 ? 0x0002 : 0) |
	    (t->fixed ? 0x0004 : 0));
	uint64_t nodes = t->node_count - t->removed + t->added + 1;

	memset(blk, 0, bs);
	for (i = 0; i < n->nkeys; i++) {
		if (!t->fixed) {
			if ((n->level == 0 && (n->e[i].kl < 8 || n->e[i].vl == 0)) ||
			    (n->level > 0 && i > 0 && n->e[i].kl < 8) ||
			    (n->level > 0 && n->e[i].vl != 8)) {
				return EIO;
			}
		}
		memcpy(blk + key_base + koff, n->e[i].k, n->e[i].kl);
		voff += n->e[i].vl;
		memcpy(blk + val_end - voff, n->e[i].v, n->e[i].vl);
		if (t->fixed) {
			if (56 + 4 * (size_t)(i + 1) > key_base) {
				return EIO;
			}
			blk[56 + 4 * i] = (uint8_t)koff;
			blk[56 + 4 * i + 1] = (uint8_t)(koff >> 8);
			blk[56 + 4 * i + 2] = (uint8_t)voff;
			blk[56 + 4 * i + 3] = (uint8_t)(voff >> 8);
		} else {
			if (56 + 8 * (size_t)(i + 1) > key_base) {
				return EIO;
			}
			blk[56 + 8 * i] = (uint8_t)koff;
			blk[56 + 8 * i + 1] = (uint8_t)(koff >> 8);
			blk[56 + 8 * i + 2] = (uint8_t)n->e[i].kl;
			blk[56 + 8 * i + 3] = (uint8_t)(n->e[i].kl >> 8);
			blk[56 + 8 * i + 4] = (uint8_t)voff;
			blk[56 + 8 * i + 5] = (uint8_t)(voff >> 8);
			blk[56 + 8 * i + 6] = (uint8_t)n->e[i].vl;
			blk[56 + 8 * i + 7] = (uint8_t)(n->e[i].vl >> 8);
		}
		koff += n->e[i].kl;
	}
	if (voff > val_end || key_base + koff > val_end - voff) {
		return EIO;
	}
	free_bytes = val_end - voff - (key_base + koff);
	blk[32] = (uint8_t)flags;
	blk[33] = (uint8_t)(flags >> 8);
	blk[34] = (uint8_t)n->level;
	blk[35] = (uint8_t)(n->level >> 8);
	b_put32(blk + 36, n->nkeys);
	blk[40] = 0;
	blk[41] = 0;
	blk[42] = (uint8_t)toc_len;
	blk[43] = (uint8_t)(toc_len >> 8);
	b_put32(blk + 44, (uint32_t)koff | ((uint32_t)free_bytes << 16));
	blk[48] = 0xff;
	blk[49] = 0xff;
	blk[50] = 0;
	blk[51] = 0;
	blk[52] = 0xff;
	blk[53] = 0xff;
	blk[54] = 0;
	blk[55] = 0;
	if (root) {
		uint8_t *info = blk + bs - 40;

		b_put32(info, t->tree_flags);
		b_put32(info + 4, bs);
		b_put32(info + 8, t->fixed ? t->key_size : 0);
		b_put32(info + 12, t->fixed ? t->val_size : 0);
		b_put32(info + 16, t->longest_key);
		b_put32(info + 20, t->longest_val);
		b_put64(info + 24, t->key_count);
		b_put64(info + 32, nodes);
		apfs_txn_seal(blk, bs, oid, apfs_txn_xid(t->txn), (t->physical ? APFS_OBJ_PHYSICAL :
		    APFS_OBJ_VIRTUAL) | APFS_OBJECT_TYPE_BTREE, t->subtype);
	} else {
		apfs_txn_seal(blk, bs, oid, apfs_txn_xid(t->txn), (t->physical ? APFS_OBJ_PHYSICAL :
		    APFS_OBJ_VIRTUAL) | APFS_OBJECT_TYPE_BTREE_NODE, t->subtype);
	}
	return 0;
}

/* ------------------------------------------------------------------------- opening */

int
btp_open(struct btp_tree *t, struct apfs_txn *txn, uint64_t root, apfs_oid_t root_oid,
    bool fixed, uint32_t key_size, uint32_t val_size, uint32_t tree_flags, uint32_t subtype,
    int (*cmp)(const uint8_t *, uint16_t, const uint8_t *, uint16_t),
    int (*resolve)(void *ctx, apfs_oid_t oid, uint64_t *addr, apfs_xid_t *key_xid),
    void *resolve_ctx, uint64_t next_oid)
{
	uint8_t *copy = NULL;
	uint32_t off;
	int error;

	apfs_where = "btp_open";
	memset(t, 0, sizeof(*t));
	t->txn = txn;
	t->bs = apfs_txn_block_size(txn);
	t->fixed = fixed;
	t->physical = (tree_flags & 0x10) != 0;
	t->key_size = key_size;
	t->val_size = val_size;
	t->tree_flags = tree_flags;
	t->root_oid = (tree_flags & 0x10) != 0 ? root : root_oid;  /* a physical root is its block */
	t->next_oid = next_oid;
	/*
	 * The root keeps its identifier for the life of the tree, and a counter that lags behind
	 * it would hand that same identifier to a new node below the root -- the tree would then
	 * name itself as a child.  An image made by an earlier mkfs is exactly such a counter.
	 */
	if (!t->physical && t->next_oid <= t->root_oid) {
		t->next_oid = t->root_oid + 1;
	}
	t->subtype = subtype;
	t->cmp = cmp;
	t->resolve = resolve;
	t->resolve_ctx = resolve_ctx;
	t->new_root = root;
	t->new_root_oid = root_oid;
	error = apfs_txn_copy_block(txn, root, &copy);
	if (error) {
		return error;
	}
	if ((b_le32(copy + 24) & APFS_OBJECT_TYPE_MASK) != APFS_OBJECT_TYPE_BTREE) {
		apfs_log("inode insert: block %llu is not a tree root (type %#x)\n",
		    (unsigned long long)root, b_le32(copy + 24));
		apfs_free(copy, t->bs);
		return EIO;
	}
	off = t->bs - 40;
	t->key_count = b_le64(copy + off + 24);
	t->node_count = b_le64(copy + off + 32);
	t->longest_key = b_le32(copy + off + 16);
	t->longest_val = b_le32(copy + off + 20);
	if (!t->physical && resolve == NULL) {
		apfs_free(copy, t->bs);
		return EINVAL;
	}
	if (!t->physical) {
		uint64_t mapped = 0;

		error = t->resolve(t->resolve_ctx, t->root_oid, &mapped, &t->root_xid);
		if (error) {
			apfs_free(copy, t->bs);
			return error;
		}
	}
	apfs_free(copy, t->bs);
	return 0;
}

void
btp_close(struct btp_tree *t)
{
	if (t->changes != NULL) {
		apfs_free(t->changes, t->cchanges * sizeof(*t->changes));
		t->changes = NULL;
	}
}

uint64_t
btp_next_oid(struct btp_tree *t)
{
	return t->next_oid;
}

void
btp_set_next_oid(struct btp_tree *t, uint64_t next)
{
	t->next_oid = next;
}

uint64_t
btp_key_count(struct btp_tree *t)
{
	return t->key_count;
}

uint64_t
btp_node_count(struct btp_tree *t)
{
	return t->node_count;
}

uint32_t
btp_change_count(struct btp_tree *t)
{
	return t->nchanges;
}

int
btp_change_at(struct btp_tree *t, uint32_t i, apfs_oid_t *oid, uint64_t *addr)
{
	if (i >= t->nchanges) {
		return ENOENT;
	}
	*oid = t->changes[i].oid;
	*addr = t->changes[i].addr;
	return 0;
}

int
btp_change_old_at(struct btp_tree *t, uint32_t i, apfs_oid_t *old_oid, apfs_xid_t *old_xid)
{
	if (i >= t->nchanges) {
		return ENOENT;
	}
	*old_oid = t->changes[i].old_oid;
	*old_xid = t->changes[i].old_xid;
	return 0;
}

/* -------------------------------------------------------------- one change, bottom up */

#define BTP_INSERT  1
#define BTP_DELETE  2
#define BTP_REPLACE 3

struct btp_edit {
	int op;
	const uint8_t *k;
	uint16_t kl;
	const uint8_t *v;
	uint16_t vl;
};

/* A node can need more than two of itself: one record may be big enough to fill a node, with
 * records on both sides of it.  These are the nodes after the first, which the parent takes. */
#define BTP_MAX_PARTS 4

/*
 * How deep a tree of this writer may be.  Every level is a frame of btp_update_node with a
 * node's worth of locals, and the kernel stack is 16 KB: sixteen levels (what the first limit
 * was) walk off the end of it on a tree that points at itself, and the panic is "Invalid
 * kernel stack pointer".  A file-system tree on these images is three or four levels; six is
 * far more than any of them has, and even a cycle in the tree stops well inside the stack.
 */
#define BTP_MAX_DEPTH 6

struct btp_out {
	uint64_t   addr;
	apfs_oid_t oid;
	uint8_t   *first;
	uint16_t   first_len;
	uint32_t   nparts;
	uint8_t   *part_key[BTP_MAX_PARTS];
	uint16_t   part_key_len[BTP_MAX_PARTS];
	uint64_t   part_addr[BTP_MAX_PARTS];
	apfs_oid_t part_oid[BTP_MAX_PARTS];
};

static void
btp_out_parts_free(struct btp_out *out)
{
	uint32_t i;

	for (i = 0; i < out->nparts && i < BTP_MAX_PARTS; i++) {
		if (out->part_key[i] != NULL) {
			apfs_free(out->part_key[i],
			    out->part_key_len[i] != 0 ? out->part_key_len[i] : 1);
			out->part_key[i] = NULL;
			out->part_key_len[i] = 0;
		}
	}
	out->nparts = 0;
}

static uint32_t
btp_lower(struct btp_tree *t, struct btp_node *n, const uint8_t *k, uint16_t kl)
{
	uint32_t lo = 0, hi = n->nkeys;

	while (lo < hi) {
		uint32_t mid = lo + (hi - lo) / 2;

		if (t->cmp(n->e[mid].k, n->e[mid].kl, k, kl) < 0) {
			lo = mid + 1;
		} else {
			hi = mid;
		}
	}
	return lo;
}

static int
btp_out_first(struct btp_out *out, const uint8_t *k, uint16_t kl)
{
	if (out->first != NULL) {
		apfs_free(out->first, out->first_len != 0 ? out->first_len : 1);
		out->first = NULL;
		out->first_len = 0;
	}
	if (k == NULL) {
		return 0;
	}
	out->first = apfs_alloc(kl != 0 ? kl : 1);
	if (out->first == NULL) {
		return ENOMEM;
	}
	memcpy(out->first, k, kl);
	out->first_len = kl;
	return 0;
}

/*
 * What the object map is told about one node: the identifier it now has and where it now is,
 * and the record that leaves the map.  A node changed twice in one transaction has one record
 * in the map, and the key it retires is the one from the first change: a split's new halves
 * retire the old node's key, not a key of their own.
 */
static int
btp_change_append(struct btp_tree *t, apfs_oid_t oid, uint64_t addr, apfs_oid_t old_oid,
    apfs_xid_t old_xid)
{
	if (t->nchanges == t->cchanges) {
		uint32_t grow = t->cchanges != 0 ? t->cchanges * 2 : 16;
		struct btp_map_change *p = apfs_alloc(grow * sizeof(*p));

		if (p == NULL) {
			return ENOMEM;
		}
		if (t->changes != NULL) {
			memcpy(p, t->changes, t->nchanges * sizeof(*p));
			apfs_free(t->changes, t->cchanges * sizeof(*p));
		}
		t->changes = p;
		t->cchanges = grow;
	}
	t->changes[t->nchanges].oid = oid;
	t->changes[t->nchanges].addr = addr;
	t->changes[t->nchanges].old_oid = old_oid;
	t->changes[t->nchanges].old_xid = old_xid;
	t->nchanges++;
	return 0;
}

static int
btp_change_note(struct btp_tree *t, apfs_oid_t oid, uint64_t addr, apfs_oid_t old_oid,
    apfs_xid_t old_xid)
{
	uint32_t i;

	if (t->physical) {
		return 0;
	}
	for (i = 0; i < t->nchanges; i++) {
		if (t->changes[i].oid == oid) {
			t->changes[i].addr = addr;
			return 0;
		}
	}
	return btp_change_append(t, oid, addr, old_oid, old_xid);
}

/*
 * And what it is told about a node that is gone altogether: only the record leaves the map, and
 * no record takes its place.  oid 0 is what says so to the caller that writes the map (and no
 * live node has that identifier); a node made and unmade in one transaction has no record to
 * retire, and old_xid 0 says that.
 */
static int
btp_change_gone(struct btp_tree *t, apfs_oid_t oid, apfs_oid_t old_oid, apfs_xid_t old_xid)
{
	uint32_t i;

	if (t->physical) {
		return 0;
	}
	for (i = 0; i < t->nchanges; i++) {
		if (t->changes[i].oid == oid) {
			t->changes[i].oid = 0;
			t->changes[i].addr = 0;
			return 0;
		}
	}
	return btp_change_append(t, 0, 0, old_oid, old_xid);
}

static int
btp_emit(struct btp_tree *t, struct btp_node *n, bool root, apfs_oid_t oid, uint64_t addr,
    apfs_oid_t old_oid, apfs_xid_t old_xid, uint64_t *out_addr, apfs_oid_t *out_oid)
{
	uint8_t *blk = apfs_alloc(t->bs);
	int error;

	if (blk == NULL) {
		return ENOMEM;
	}
	error = btp_node_pack(t, n, root, oid, blk);
	if (error == 0) {
		error = apfs_txn_stage_block(t->txn, addr, blk);
	}
	apfs_free(blk, t->bs);
	if (error) {
		return error;
	}
	if (!t->physical && oid < 0x400) {
		apfs_log("PADDR LEAK: subtype 0x%x emit block %llu oid %llu root %d\n", t->subtype,
		    (unsigned long long)addr, (unsigned long long)oid, (int)root);
	}
	t->added++;
	*out_addr = addr;
	*out_oid = oid;
	return btp_change_note(t, oid, addr, old_oid, old_xid);
}

/*
 * Where the node with this identifier is: the nodes this transaction has already written come
 * first -- their entries in the object map are only made when the whole change is done, and a
 * second change to the same tree still has to find them.
 */
static int
btp_resolve(struct btp_tree *t, apfs_oid_t oid, uint64_t *addr, apfs_xid_t *key_xid)
{
	uint32_t i;

	for (i = 0; i < t->nchanges; i++) {
		if (t->changes[i].oid == oid) {
			*addr = t->changes[i].addr;
			if (key_xid != NULL) {
				*key_xid = t->changes[i].old_xid;
			}
			return 0;
		}
	}
	if (t->resolve == NULL) {
		return ENOENT;
	}
	return t->resolve(t->resolve_ctx, oid, addr, key_xid);
}

static apfs_oid_t
btp_new_oid(struct btp_tree *t, uint64_t addr)
{
	if (!t->physical) {
		/* below 1024 the identifiers are the container's own, and a node may not take one */
		if (t->next_oid < APFS_OID_RESERVED_COUNT) {
			t->next_oid = APFS_OID_RESERVED_COUNT;
		}
		/* an operation that names its own new object may set the counter below the root's */
		if (t->next_oid <= t->root_oid) {
			t->next_oid = t->root_oid + 1;
		}
		return t->next_oid++;
	}
	return addr;
}

static uint64_t btp_path[BTP_MAX_DEPTH + 1];

static int
btp_update_node(struct btp_tree *t, uint64_t addr, apfs_oid_t oid, apfs_xid_t oid_xid, bool root,
    const struct btp_edit *ed, struct btp_out *out, int depth, uint16_t expect)
{
	struct btp_node n;
	uint32_t pos;
	int error;

	if (depth >= 0 && depth <= BTP_MAX_DEPTH) {
		btp_path[depth] = addr;
	}
	if (depth >= BTP_MAX_DEPTH) {
		unsigned i;

		apfs_log("tree too deep at block %llu (level %u), path:");
		for (i = 0; i <= (unsigned)BTP_MAX_DEPTH; i++) {
			apfs_log(" %llu", (unsigned long long)btp_path[i]);
		}
		apfs_log("\n");
		apfs_log("tree too deep at block %llu (level %u)\n",
		    (unsigned long long)addr, depth);
		return EIO;
	}
	memset(out, 0, sizeof(*out));
	error = btp_node_load(t, addr, &n);
	if (error) {
		return error;
	}
	/*
	 * A child's level is its parent's minus one.  When it is not, the parent points at a node
	 * of its own height or higher -- a cycle in the tree -- and the descent that follows walks
	 * down it until the depth guard stops it.  p321/p323 had the guard firing on the same
	 * block over and over with nothing else said; this says it at the first step.
	 */
	if (expect != 0xffff && n.level != expect) {
		apfs_log("tree: node %llu is level %u, the parent said %u (depth %d)\n",
		    (unsigned long long)addr, (unsigned)n.level, (unsigned)expect, depth);
	}

	if (n.level == 0) {
		pos = btp_lower(t, &n, ed->k, ed->kl);
		if (ed->op == BTP_INSERT) {
			if (pos < n.nkeys && t->cmp(n.e[pos].k, n.e[pos].kl, ed->k, ed->kl) == 0) {
				btp_node_free(&n);
				return EEXIST;
			}
			error = btp_entry_grow(&n, n.nkeys + 1);
			if (error) {
				btp_node_free(&n);
				return error;
			}
			memmove(&n.e[pos + 1], &n.e[pos], (n.nkeys - pos) * sizeof(*n.e));
			n.e[pos].k = apfs_alloc(ed->kl != 0 ? ed->kl : 1);
			n.e[pos].v = apfs_alloc(ed->vl != 0 ? ed->vl : 1);
			if (n.e[pos].k == NULL || n.e[pos].v == NULL) {
				n.nkeys++;
				btp_node_free(&n);
				return ENOMEM;
			}
			memcpy(n.e[pos].k, ed->k, ed->kl);
			memcpy(n.e[pos].v, ed->v, ed->vl);
			n.e[pos].kl = ed->kl;
			n.e[pos].vl = ed->vl;
			n.nkeys++;
			t->key_count++;
			if (ed->kl > t->longest_key) {
				t->longest_key = ed->kl;
			}
			if (ed->vl > t->longest_val) {
				t->longest_val = ed->vl;
			}
		} else {
			if (pos >= n.nkeys || t->cmp(n.e[pos].k, n.e[pos].kl, ed->k, ed->kl) != 0) {
				btp_node_free(&n);
				return ENOENT;
			}
			if (ed->op == BTP_DELETE) {
				apfs_free(n.e[pos].k, n.e[pos].kl != 0 ? n.e[pos].kl : 1);
				apfs_free(n.e[pos].v, n.e[pos].vl != 0 ? n.e[pos].vl : 1);
				memmove(&n.e[pos], &n.e[pos + 1], (n.nkeys - pos - 1) * sizeof(*n.e));
				n.nkeys--;
				t->key_count--;
			} else {
				apfs_free(n.e[pos].v, n.e[pos].vl != 0 ? n.e[pos].vl : 1);
				n.e[pos].v = apfs_alloc(ed->vl != 0 ? ed->vl : 1);
				if (n.e[pos].v == NULL) {
					btp_node_free(&n);
					return ENOMEM;
				}
				memcpy(n.e[pos].v, ed->v, ed->vl);
				n.e[pos].vl = ed->vl;
				if (ed->vl > t->longest_val) {
					t->longest_val = ed->vl;
				}
			}
		}
	} else {
		struct btp_out co;
		apfs_oid_t child_oid;
		uint64_t child_addr = 0;
		apfs_xid_t child_xid = 0;

		pos = btp_lower(t, &n, ed->k, ed->kl);
		/*
		 * An entry's key is the first key of the subtree it names: the child to go into
		 * is the one that entry names when the key is that very first key, and the one
		 * before it otherwise.
		 */
		if (pos >= n.nkeys) {
			pos = n.nkeys - 1;
		} else if (pos > 0 && t->cmp(n.e[pos].k, n.e[pos].kl, ed->k, ed->kl) != 0) {
			pos--;
		}
		if (n.nkeys == 0 || n.e[pos].vl != 8) {
			apfs_log("an entry of %u bytes at %u of a node of level %u: not a child\n",
			    (unsigned)n.e[pos].vl, pos, (unsigned)n.level);
			btp_node_free(&n);
			return EIO;
		}
		child_oid = b_le64(n.e[pos].v);
		if (t->physical) {
			child_addr = child_oid;
			child_xid = 0;
		} else if (btp_resolve(t, child_oid, &child_addr, &child_xid) != 0) {
			apfs_log("inode insert: child oid %llu of block %llu is not in the object map\n",
			    (unsigned long long)child_oid, (unsigned long long)addr);
			btp_node_free(&n);
			return EIO;
		}
		error = btp_update_node(t, child_addr, child_oid, child_xid, false, ed, &co,
		    depth + 1, (uint16_t)(n.level > 0 ? n.level - 1 : 0));
		if (error) {
			btp_node_free(&n);
			return error;
		}
		if (co.oid == 0 && co.nparts == 0) {
			/*
			 * The child held nothing and is gone: the entry that names it goes too.  A
			 * child left empty in place would be one apfsck reports -- "empty children
			 * should just be deleted" -- and one whose key still claims a range of the
			 * tree that is now in the leaf before it.
			 */
			apfs_free(n.e[pos].k, n.e[pos].kl != 0 ? n.e[pos].kl : 1);
			apfs_free(n.e[pos].v, n.e[pos].vl != 0 ? n.e[pos].vl : 1);
			memmove(&n.e[pos], &n.e[pos + 1], (n.nkeys - pos - 1) * sizeof(*n.e));
			n.nkeys--;
			apfs_free(co.first, co.first_len != 0 ? co.first_len : 1);
			goto node_done;
		}
		/* the entry for that child: its new identifier, and its key if it moved */
		apfs_free(n.e[pos].v, n.e[pos].vl != 0 ? n.e[pos].vl : 1);
		n.e[pos].v = apfs_alloc(8);
		if (n.e[pos].v == NULL) {
			apfs_free(co.first, co.first_len != 0 ? co.first_len : 1);
			btp_out_parts_free(&co);
			btp_node_free(&n);
			return ENOMEM;
		}
		b_put64(n.e[pos].v, co.oid);
		n.e[pos].vl = 8;
		if (co.first != NULL) {
			apfs_free(n.e[pos].k, n.e[pos].kl != 0 ? n.e[pos].kl : 1);
			n.e[pos].k = co.first;
			n.e[pos].kl = co.first_len;
			co.first = NULL;
		}
		if (co.nparts != 0) {
			uint32_t j;

			error = btp_entry_grow(&n, n.nkeys + co.nparts);
			if (error) {
				btp_out_parts_free(&co);
				btp_node_free(&n);
				return error;
			}
			memmove(&n.e[pos + 1 + co.nparts], &n.e[pos + 1],
			    (n.nkeys - pos - 1) * sizeof(*n.e));
			for (j = 0; j < co.nparts; j++) {
				n.e[pos + 1 + j].k = co.part_key[j];
				n.e[pos + 1 + j].kl = co.part_key_len[j];
				co.part_key[j] = NULL;
				n.e[pos + 1 + j].v = apfs_alloc(8);
				if (n.e[pos + 1 + j].v == NULL) {
					apfs_free(n.e[pos + 1 + j].k,
					    n.e[pos + 1 + j].kl != 0 ? n.e[pos + 1 + j].kl : 1);
					n.e[pos + 1 + j].k = NULL;
					btp_out_parts_free(&co);
					btp_node_free(&n);
					return ENOMEM;
				}
				b_put64(n.e[pos + 1 + j].v, co.part_oid[j]);
				n.e[pos + 1 + j].vl = 8;
				co.part_key[j] = NULL;
				n.nkeys++;
			}
			btp_out_parts_free(&co);
		}
		apfs_free(co.first, co.first_len != 0 ? co.first_len : 1);
	}

node_done:
	/*
	 * A node with nothing left in it is not written: the reader that checks this container
	 * wants an empty child deleted, not written as an empty one.  The parent has already
	 * dropped the entry that named it (the oid 0 this returns says so), so the tree only
	 * loses a level or a branch.  The root is the exception -- an empty tree is an empty
	 * leaf -- and it is handled below.
	 */
	if (n.nkeys == 0 && !root) {
		btp_node_free(&n);
		error = apfs_txn_free_block_who(t->txn, addr, APFS_FREE_NODE, oid);
		if (error) {
			return error;
		}
		t->removed++;
		return btp_change_gone(t, oid, oid, oid_xid);
	}
	if (root && n.nkeys == 0) {
		n.level = 0;
	}

	/* this node is replaced: its block goes back, a new one is written in its place */
	error = apfs_txn_free_block_who(t->txn, addr, APFS_FREE_NODE, oid);
	if (error) {
		btp_node_free(&n);
		return error;
	}
	t->removed++;

	if (btp_node_bytes(t, &n, root) <= t->bs) {
		uint64_t at;
		apfs_oid_t noid;

		error = apfs_txn_alloc_block(t->txn, &at);
		if (error) {
			btp_node_free(&n);
			return error;
		}
		if (root) {
			noid = t->physical ? at : t->root_oid;
		} else if (t->physical) {
			noid = at;
		} else {
			noid = oid != 0 ? oid : btp_new_oid(t, at);
		}
		error = btp_emit(t, &n, root, noid, at, oid, oid_xid, &out->addr, &out->oid);
		if (error) {
			btp_node_free(&n);
			return error;
		}
		if (n.nkeys != 0) {
			error = btp_out_first(out, n.e[0].k, n.e[0].kl);
		}
		btp_node_free(&n);
		return error;
	}

	/*
	 * The node does not fit: cut it into as many nodes as it takes, each as full as it can
	 * be.  Usually that is two.  One record can be big enough -- an attribute's value --
	 * to need a node of its own with records on both sides of it, and then it is three.
	 * None of the parts is the root, so none carries a btree_info.
	 */
	{
		struct btp_node part[BTP_MAX_PARTS];
		uint32_t nparts = 0, from = 0, i, target = 0;

		/*
		 * Cut into parts of the same size, not into one full part and a remainder: filling
		 * the first part to the last byte leaves the second with a record or two, every
		 * following insert goes into that empty one, and it splits the same way -- so each
		 * split adds a level's worth of nearly empty nodes and the tree grows in height
		 * instead of width (p321: 62 478 records in 4 657 nodes, five levels, four and a
		 * bit children a node; 21 000 records at depth 3 on the host in 400 creates).
		 */
		{
			uint32_t total = btp_node_bytes(t, &n, false);
			uint32_t want = (total + t->bs - 1) / t->bs;

			if (want < 2) {
				want = 2;
			}
			target = total / want;
		}
		while (from < n.nkeys) {
			struct btp_node p;
			uint32_t cnt = 0;

			if (nparts == BTP_MAX_PARTS) {
				btp_node_free(&n);
				return ENOSPC;
			}
			memset(&p, 0, sizeof(p));
			p.e = n.e + from;
			p.level = n.level;
			for (i = from; i < n.nkeys; i++) {
				uint32_t bytes;

				p.nkeys = i - from + 1;
				bytes = btp_node_bytes(t, &p, false);
				if (bytes > t->bs) {
					break;
				}
				cnt = p.nkeys;
				if (bytes >= target) {
					break;
				}
			}
			if (cnt == 0) {
				btp_node_free(&n);      /* one record larger than a node */
				return ENOSPC;
			}
			memset(&part[nparts], 0, sizeof(part[nparts]));
			part[nparts].e = n.e + from;
			part[nparts].nkeys = cnt;
			part[nparts].level = n.level;
			from += cnt;
			nparts++;
		}

		/*
		 * A root carries forty bytes more than a part does; a node that only just missed
		 * fitting as the root may fill one part exactly.  It cannot stay one node (it is
		 * about to be a child of a new root), so cut its last record off.
		 */
		if (root && nparts == 1) {
			if (part[0].nkeys < 2) {
				btp_node_free(&n);
				return ENOSPC;
			}
			part[1] = part[0];
			part[1].e = part[0].e + part[0].nkeys - 1;
			part[1].nkeys = 1;
			part[0].nkeys -= 1;
			nparts = 2;
		}

		/* every part is a new object; the first keeps the node's own identifier */
		out->nparts = nparts - 1;
		for (i = 0; i < nparts; i++) {
			uint64_t at = 0;
			apfs_oid_t noid;
			bool keep_oid = (i == 0 && !root);
			uint64_t *out_addr = i == 0 ? &out->addr : &out->part_addr[i - 1];
			apfs_oid_t *out_oid = i == 0 ? &out->oid : &out->part_oid[i - 1];

			error = apfs_txn_alloc_block(t->txn, &at);
			if (error == 0) {
				/*
				 * A node the parent points at keeps its identifier; the parts of a
				 * root do not -- each is a new child, and the root's own record is
				 * retired when the new root is written.
				 */
				noid = keep_oid && !t->physical ? (oid != 0 ? oid :
				    btp_new_oid(t, at)) : btp_new_oid(t, at);
				error = btp_emit(t, &part[i], false, noid, at,
				    keep_oid ? oid : 0, keep_oid ? oid_xid : 0, out_addr, out_oid);
			}
			if (error) {
				btp_out_parts_free(out);
				btp_node_free(&n);
				return error;
			}
		}
		/* the parent's entry for every part after the first takes its first key */
		for (i = 1; i < nparts; i++) {
			uint16_t kl = part[i].e[0].kl;

			out->part_key[i - 1] = apfs_alloc(kl != 0 ? kl : 1);
			if (out->part_key[i - 1] == NULL) {
				btp_out_parts_free(out);
				btp_node_free(&n);
				return ENOMEM;
			}
			memcpy(out->part_key[i - 1], part[i].e[0].k, kl);
			out->part_key_len[i - 1] = kl;
		}
		error = btp_out_first(out, part[0].e[0].k, part[0].e[0].kl);
		btp_node_free(&n);      /* the parts' entries point into n.e */
		return error;
	}
}

/*
 * One change to the tree.  BTP_INSERT refuses a key that is there, BTP_DELETE and BTP_REPLACE
 * one that is not; either way the tree is left as it was on an error.
 */
int
btp_update(struct btp_tree *t, int op, const uint8_t *key, uint16_t klen, const uint8_t *val,
    uint16_t vlen)
{
	struct btp_edit ed;
	struct btp_out out;
	int error;

	ed.op = op;
	ed.k = key;
	ed.kl = klen;
	ed.v = val;
	ed.vl = vlen;
	/* the node count of this one change, not of every change the tree has had */
	t->removed = 0;
	t->added = 0;
	error = btp_update_node(t, t->new_root, t->new_root_oid, t->root_xid, true, &ed, &out,
	    0, 0xffff);
	if (error) {
		return error;
	}
	if (out.nparts != 0) {
		/* the root grew: a new root, same identifier, one entry per part */
		struct btp_node r;
		struct btp_entry e[BTP_MAX_PARTS + 1];
		uint32_t cnt = out.nparts + 1, i;
		uint64_t at = 0;
		apfs_oid_t noid;

		memset(&r, 0, sizeof(r));
		memset(e, 0, sizeof(e));
		e[0].k = out.first;
		e[0].kl = out.first_len;
		for (i = 0; i < cnt; i++) {
			e[i].v = apfs_alloc(8);
			if (e[i].v == NULL) {
				error = ENOMEM;
				goto root_out;
			}
			e[i].vl = 8;            /* a child is named by its identifier */
		}
		b_put64(e[0].v, out.oid);
		for (i = 1; i < cnt; i++) {
			e[i].k = out.part_key[i - 1];
			e[i].kl = out.part_key_len[i - 1];
			b_put64(e[i].v, out.part_oid[i - 1]);
		}
		r.nkeys = cnt;
		r.e = e;
		/* the new root is one level above the parts */
		{
			struct btp_node child;

			error = btp_node_load(t, out.addr, &child);
			if (error) {
				goto root_out;
			}
			r.level = (uint16_t)(child.level + 1);
			btp_node_free(&child);
		}
		error = apfs_txn_alloc_block(t->txn, &at);
		if (error) {
			goto root_out;
		}
		noid = t->root_oid;
		error = btp_emit(t, &r, true, noid, at, t->root_oid, t->root_xid,
		    &t->new_root, &t->new_root_oid);
root_out:
		for (i = 0; i < cnt; i++) {
			if (e[i].v != NULL) {
				apfs_free(e[i].v, 8);
			}
			if (i > 0 && e[i].k != NULL) {
				apfs_free(e[i].k, e[i].kl != 0 ? e[i].kl : 1);
				out.part_key[i - 1] = NULL;
			}
		}
		if (out.first != NULL) {
			apfs_free(out.first, out.first_len != 0 ? out.first_len : 1);
			out.first = NULL;
		}
		btp_out_parts_free(&out);
		if (error) {
			return error;
		}
	} else {
		if (!t->physical && out.oid < 0x400) {
			apfs_log("PADDR LEAK: subtype 0x%x update root oid %llu addr %llu\n",
			    t->subtype, (unsigned long long)out.oid,
			    (unsigned long long)out.addr);
		}
		t->new_root = out.addr;
		t->new_root_oid = out.oid;
		apfs_free(out.first, out.first_len != 0 ? out.first_len : 1);
	}
	t->node_count = t->node_count - t->removed + t->added;
	return 0;
}

int
btp_root(struct btp_tree *t, uint64_t *addr, apfs_oid_t *oid)
{
	*addr = t->new_root;
	*oid = t->new_root_oid;
	return 0;
}
