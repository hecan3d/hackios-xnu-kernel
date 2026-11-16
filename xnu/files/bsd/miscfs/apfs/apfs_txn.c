/*
 * Writing into APFS: one transaction.
 *
 * apfs_write.c replaces bytes inside a file and touches no structure of the container.  This
 * is the other half: a change to the metadata, made the way APFS makes one -- fresh copies of
 * everything from the changed record up to the container superblock, in blocks that were free,
 * published by one block written last.  What it changes here is one field of one inode (its
 * modification time): the smallest change that is still a change of metadata, and the one
 * tools/apfs/txn-probe.py does first on the computer.
 *
 * What a transaction touches, from the bottom up, and in the order it has to be written:
 *
 *   1. the file-system tree's nodes, the leaf that holds the record and every node above it
 *   2. the volume's object map: an entry per new node, keyed (object id, transaction)
 *   3. the volume superblock: the object map it names
 *   4. the container's object map: the entry for the volume superblock
 *   5. the space manager: the chunk bitmaps and chunk-info blocks it changed, the internal
 *      pool's own bitmap, the totals, and the new addresses in its array
 *   6. the reaper and the free queues, the checkpoint's other ephemeral objects, copied into
 *      the checkpoint data ring behind the space manager
 *   7. the checkpoint map: where each of those landed
 *   8. the container superblock, in the descriptor ring, with a larger transaction id -- the
 *      commit
 *   9. block zero, the copy a clean unmount leaves (not part of the commit)
 *
 * What it does not do, yet, and each of these is a layer of its own:
 *
 *   - trees of more than one node: a leaf that is the root is the shape every image here is
 *     made in, and splitting or merging nodes is the next file
 *   - the free queues: blocks are marked free in the chunk bitmap the moment the transaction
 *     that owned them is superseded, instead of going on a queue keyed by transaction.  The
 *     live checkpoint is consistent and apfsck agrees; the one before it is not safe to go
 *     back to, which is a rollback this port does not offer yet
 *   - several devices (Fusion), chunk-info address blocks, an internal-pool bitmap of more
 *     than one block: refused, with the reason in the log
 *   - snapshots, clones, encryption, sealing: refused by the caller, not handled here
 *
 * Every block is read back and compared before the next one goes out, and the commit block is
 * the only one whose tearing matters: until it lands whole the reader goes on taking the
 * checkpoint before it.
 */

#include <stddef.h>

#include "apfs_txn_priv.h"

/* ------------------------------------------------------- little-endian, for a writer */

static inline uint32_t
t_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static inline uint64_t
t_le64(const uint8_t *p)
{
	return (uint64_t)t_le32(p) | (uint64_t)t_le32(p + 4) << 32;
}

static inline void
t_put16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

static inline void
t_put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

static inline void
t_put64(uint8_t *p, uint64_t v)
{
	t_put32(p, (uint32_t)v);
	t_put32(p + 4, (uint32_t)(v >> 32));
}

/* An object's header and its checksum, as every block that is an object carries them. */
static void
txn_seal(uint8_t *blk, uint32_t size, apfs_oid_t oid, apfs_xid_t xid, uint32_t type,
    uint32_t subtype)
{
	t_put64(blk + 8, oid);
	t_put64(blk + 16, xid);
	t_put32(blk + 24, type);
	t_put32(blk + 28, subtype);
	t_put64(blk, apfs_fletcher64(blk + 8, size - 8));
}

/* -------------------------------------------------------------------- the transaction */

#define TXN_MAX_CHUNKS 32
#define TXN_BTNODE_ROOT      0x0001
#define TXN_BTNODE_LEAF      0x0002
#define TXN_BTNODE_FIXED_KV  0x0004

struct txn_chunk {
	uint32_t index;
	uint8_t *bm;                    /* this transaction's copy of the chunk's bitmap */
	uint32_t free;                  /* what the chunk-info record will say */
	uint64_t old_bm;                /* where its bitmap is now (0: the chunk was all free) */
	uint64_t new_bm;                /* and where this transaction puts the new one */
	bool     dirty;
};

struct txn_pending {
	uint64_t addr;
	uint8_t *data;                  /* block_size bytes, written in the order it was added */
};

struct apfs_txn {
	struct apfs_nx       *nx;
	struct apfs_spaceman *sm;
	apfs_xid_t            xid;
	struct txn_pending   *pend;
	uint32_t              npend, cpend;
	struct txn_chunk      chunks[TXN_MAX_CHUNKS];
	uint32_t              nchunks;
	uint8_t              *ipbm;     /* this transaction's copy of the pool's own bitmap */
	bool                  ip_dirty;
	uint32_t              cursor;   /* the chunk to look for free blocks in first */
	uint64_t              allocated, freed;
	/*
	 * Blocks given back are not free until the transaction has taken everything it needs:
	 * a block freed early in the change would otherwise be handed out again while the
	 * checkpoint a reader may still fall back to refers to it.  So they are queued, and
	 * the bitmaps are told at the end.
	 */
	uint64_t             *freed_blocks;
	/*
	 * Where each block given back came from, for the day one of them is given back twice:
	 * the check that refuses that can then say who gave it back, instead of a number.
	 */
	uint8_t              *freed_who;
	uint64_t             *freed_arg;
	uint32_t              nfreed_blocks, cfreed_blocks;
	uint64_t             *freed_ip;
	uint32_t              nfreed_ip, cfreed_ip;
};

static uint32_t
txn_bs(struct apfs_txn *txn)
{
	return txn->nx->block_size;
}

void
apfs_txn_abort(struct apfs_txn *txn)
{
	uint32_t i;

	if (txn == NULL) {
		return;
	}
	for (i = 0; i < txn->npend; i++) {
		apfs_free(txn->pend[i].data, txn_bs(txn));
	}
	if (txn->pend != NULL) {
		apfs_free(txn->pend, txn->cpend * sizeof(*txn->pend));
	}
	for (i = 0; i < txn->nchunks; i++) {
		apfs_free(txn->chunks[i].bm, txn_bs(txn));
	}
	if (txn->ipbm != NULL) {
		apfs_free(txn->ipbm, txn_bs(txn));
	}
	if (txn->freed_blocks != NULL) {
		apfs_free(txn->freed_blocks, txn->cfreed_blocks * sizeof(*txn->freed_blocks));
	}
	if (txn->freed_who != NULL) {
		apfs_free(txn->freed_who, txn->cfreed_blocks * sizeof(*txn->freed_who));
	}
	if (txn->freed_arg != NULL) {
		apfs_free(txn->freed_arg, txn->cfreed_blocks * sizeof(*txn->freed_arg));
	}
	if (txn->freed_ip != NULL) {
		apfs_free(txn->freed_ip, txn->cfreed_ip * sizeof(*txn->freed_ip));
	}
	apfs_free(txn, sizeof(*txn));
}

struct apfs_txn *
apfs_txn_begin(struct apfs_nx *nx, struct apfs_spaceman *sm)
{
	struct apfs_txn *txn;

	if (nx->dev.write == NULL || nx->sb == NULL) {
		return NULL;
	}
	txn = apfs_alloc(sizeof(*txn));
	if (txn == NULL) {
		return NULL;
	}
	memset(txn, 0, sizeof(*txn));
	txn->nx = nx;
	txn->sm = sm;
	txn->xid = nx->xid + 1;
	return txn;
}

/* Queue a block to be written, in this order, when the transaction commits. */
static int
txn_stage(struct apfs_txn *txn, uint64_t addr, const uint8_t *data)
{
	if (addr >= txn->nx->block_count) {
		return EIO;
	}
	if (txn->npend == txn->cpend) {
		uint32_t grow = txn->cpend != 0 ? txn->cpend * 2 : 32;
		struct txn_pending *p = apfs_alloc(grow * sizeof(*p));

		if (p == NULL) {
			return ENOMEM;
		}
		if (txn->pend != NULL) {
			memcpy(p, txn->pend, txn->npend * sizeof(*p));
			apfs_free(txn->pend, txn->cpend * sizeof(*p));
		}
		txn->pend = p;
		txn->cpend = grow;
	}
	txn->pend[txn->npend].data = apfs_alloc(txn_bs(txn));
	if (txn->pend[txn->npend].data == NULL) {
		return ENOMEM;
	}
	memcpy(txn->pend[txn->npend].data, data, txn_bs(txn));
	txn->pend[txn->npend].addr = addr;
	txn->npend++;
	return 0;
}

/* A copy of a block, for the caller to change before it stages it. */
static int
txn_copy_block(struct apfs_txn *txn, uint64_t addr, uint8_t **out)
{
	const uint8_t *blk;
	int error;

	*out = NULL;
	error = apfs_block_read(txn->nx, addr, &blk);
	if (error) {
		return error;
	}
	*out = apfs_alloc(txn_bs(txn));
	if (*out == NULL) {
		return ENOMEM;
	}
	memcpy(*out, blk, txn_bs(txn));
	return 0;
}

/* ---------------------------------------------------------------- allocating, freeing */

static int
txn_chunk_get(struct apfs_txn *txn, uint32_t index, struct txn_chunk **out)
{
	struct apfs_chunk_info ci;
	struct txn_chunk *c;
	const uint8_t *bitmap;
	uint32_t i;
	int error;

	for (i = 0; i < txn->nchunks; i++) {
		if (txn->chunks[i].index == index) {
			*out = &txn->chunks[i];
			return 0;
		}
	}
	if (txn->nchunks == TXN_MAX_CHUNKS) {
		apfs_log("a transaction touched more than %u chunks\n", TXN_MAX_CHUNKS);
		return ENOSPC;
	}
	error = apfs_spaceman_chunk(txn->sm, index, &ci);
	if (error) {
		return error;
	}
	error = apfs_spaceman_bitmap(txn->sm, &ci, &bitmap);
	if (error) {
		return error;
	}
	c = &txn->chunks[txn->nchunks];
	c->bm = apfs_alloc(txn_bs(txn));
	if (c->bm == NULL) {
		return ENOMEM;
	}
	if (bitmap != NULL) {
		memcpy(c->bm, bitmap, txn_bs(txn));
	} else {
		memset(c->bm, 0, txn_bs(txn));
	}
	c->index = index;
	c->free = ci.ci_free_count;
	c->old_bm = (uint64_t)ci.ci_bitmap_addr;
	c->new_bm = 0;
	c->dirty = false;
	txn->nchunks++;
	*out = c;
	return 0;
}

/*
 * `n` free blocks, in runs where the chunk has them, from the chunk the last allocation came
 * from first.  The bits are set as the blocks are handed out, and the free counts of the chunk
 * and of the container go down with them.
 */
static int
txn_alloc(struct apfs_txn *txn, uint32_t n, uint64_t *out)
{
	uint32_t got = 0;

	while (got < n) {
		bool found = false;
		uint32_t i;

		for (i = 0; i < txn->sm->chunk_count && !found; i++) {
			uint32_t index = (txn->cursor + i) % txn->sm->chunk_count;
			struct apfs_chunk_info ci;
			struct txn_chunk *c;
			uint32_t want, take = 0, start;

			if (txn_chunk_get(txn, index, &c) != 0 ||
			    apfs_spaceman_chunk(txn->sm, index, &ci) != 0) {
				return ENOSPC;
			}
			if (c->free == 0) {
				continue;
			}
			want = n - got;
			if (want > c->free) {
				want = c->free;
			}
			for (start = 0; start < ci.ci_block_count && take < want; ) {
				uint32_t run = 0;

				while (start + run < ci.ci_block_count && take + run < want &&
				    ((c->bm[(start + run) >> 3] >> ((start + run) & 7)) & 1) == 0) {
					run++;
				}
				if (run == 0) {
					start++;
					continue;
				}
				for (uint32_t k = 0; k < run; k++) {
					uint32_t bit = start + k;

					c->bm[bit >> 3] |= (uint8_t)(1 << (bit & 7));
					out[got++] = (uint64_t)ci.ci_addr + bit;
				}
				take += run;
				start += run;
			}
			if (take == 0) {
				continue;
			}
			c->free -= take;
			txn->sm->free_count -= take;
			txn->allocated += take;
			c->dirty = true;
			txn->cursor = index;
			found = true;
		}
		if (!found) {
			apfs_log("the container has no room for %u more blocks\n", n - got);
			return ENOSPC;
		}
	}
	return 0;
}

static int
txn_queue_simple(uint64_t **list, uint32_t *n, uint32_t *cap, uint64_t addr)
{
	if (*n == *cap) {
		uint32_t grow = *cap != 0 ? *cap * 2 : 32;
		uint64_t *p = apfs_alloc(grow * sizeof(*p));

		if (p == NULL) {
			return ENOMEM;
		}
		if (*list != NULL) {
			memcpy(p, *list, *n * sizeof(*p));
			apfs_free(*list, *cap * sizeof(*p));
		}
		*list = p;
		*cap = grow;
	}
	(*list)[(*n)++] = addr;
	return 0;
}

static int
txn_queue_free(struct apfs_txn *txn, uint64_t addr, uint8_t who, uint64_t arg)
{
	if (txn->nfreed_blocks == txn->cfreed_blocks) {
		uint32_t grow = txn->cfreed_blocks != 0 ? txn->cfreed_blocks * 2 : 32;
		uint64_t *p = apfs_alloc(grow * sizeof(*p));
		uint8_t *w = apfs_alloc(grow * sizeof(*w));
		uint64_t *a = apfs_alloc(grow * sizeof(*a));

		if (p == NULL || w == NULL || a == NULL) {
			apfs_free(p, grow * sizeof(*p));
			apfs_free(w, grow * sizeof(*w));
			apfs_free(a, grow * sizeof(*a));
			return ENOMEM;
		}
		if (txn->freed_blocks != NULL) {
			memcpy(p, txn->freed_blocks, txn->nfreed_blocks * sizeof(*p));
			memcpy(w, txn->freed_who, txn->nfreed_blocks * sizeof(*w));
			memcpy(a, txn->freed_arg, txn->nfreed_blocks * sizeof(*a));
			apfs_free(txn->freed_blocks, txn->cfreed_blocks * sizeof(*p));
			apfs_free(txn->freed_who, txn->cfreed_blocks * sizeof(*w));
			apfs_free(txn->freed_arg, txn->cfreed_blocks * sizeof(*a));
		}
		txn->freed_blocks = p;
		txn->freed_who = w;
		txn->freed_arg = a;
		txn->cfreed_blocks = grow;
	}
	txn->freed_blocks[txn->nfreed_blocks] = addr;
	txn->freed_who[txn->nfreed_blocks] = who;
	txn->freed_arg[txn->nfreed_blocks] = arg;
	txn->nfreed_blocks++;
	return 0;
}

const char *
apfs_free_who_name(uint8_t who)
{
	switch (who) {
	case APFS_FREE_NODE:
		return "a tree node";
	case APFS_FREE_EXTENT:
		return "a file extent";
	case APFS_FREE_TRUNC:
		return "the tail of a truncated file";
	case APFS_FREE_INODE:
		return "an inode's extents";
	case APFS_FREE_COMMIT:
		return "the commit retiring an old object";
	default:
		return "an unknown caller";
	}
}

static int
txn_free(struct apfs_txn *txn, uint64_t addr, uint8_t who, uint64_t arg)
{
	uint32_t i;

	if (addr == 0 || addr >= txn->nx->block_count) {
		apfs_log("block %llu cannot be given back: %s %#llx, the container has %llu blocks\n",
		    (unsigned long long)addr, apfs_free_who_name(who), (unsigned long long)arg,
		    (unsigned long long)txn->nx->block_count);
		return EIO;
	}
	/*
	 * The same block twice in one transaction is the mistake before the one the bitmaps
	 * catch at the end: say who asked twice, while both are still here.
	 */
	for (i = 0; i < txn->nfreed_blocks; i++) {
		if (txn->freed_blocks[i] == addr) {
			apfs_log("block %llu is given back twice in one transaction: %s %#llx, then %s %#llx\n",
			    (unsigned long long)addr, apfs_free_who_name(txn->freed_who[i]),
			    (unsigned long long)txn->freed_arg[i], apfs_free_who_name(who),
			    (unsigned long long)arg);
			return 0;               /* the bitmaps are told once, below */
		}
	}
	return txn_queue_free(txn, addr, who, arg);
}

/* Everything given back goes into the bitmaps now; nothing has been allocated since. */
static int
txn_apply_frees(struct apfs_txn *txn)
{
	uint32_t i;

	for (i = 0; i < txn->nfreed_blocks; i++) {
		struct txn_chunk *c;
		struct apfs_chunk_info ci;
		uint32_t chunk, bit;
		uint64_t addr = txn->freed_blocks[i];

		if (apfs_spaceman_locate(txn->sm, addr, &chunk, &bit) != 0 ||
		    txn_chunk_get(txn, chunk, &c) != 0 ||
		    apfs_spaceman_chunk(txn->sm, chunk, &ci) != 0) {
			apfs_log("block %llu: cannot be placed to be given back\n",
			    (unsigned long long)addr);
			return EIO;
		}
		if (((c->bm[bit >> 3] >> (bit & 7)) & 1) == 0) {
			apfs_log("block %llu is given back but was free already: %s %#llx asked for it\n",
			    (unsigned long long)addr, apfs_free_who_name(txn->freed_who[i]),
			    (unsigned long long)txn->freed_arg[i]);
			return EIO;
		}
		c->bm[bit >> 3] &= (uint8_t)~(1 << (bit & 7));
		c->free++;
		c->dirty = true;
		txn->sm->free_count++;
		txn->freed++;
	}
	return 0;
}

/* The internal pool's own bitmap: one block, which every image this handles has. */
static int
txn_ip_prepare(struct apfs_txn *txn)
{
	const uint8_t *bitmap;
	uint16_t slot;
	int error;

	if (txn->ipbm != NULL) {
		return 0;
	}
	if (txn->sm->ip_bm_size_in_blocks != 1) {
		apfs_log("the internal pool's bitmap is %u blocks; this writes one\n",
		    txn->sm->ip_bm_size_in_blocks);
		return ENOTSUP;
	}
	slot = (uint16_t)(txn->sm->sm[txn->sm->ip_bitmap_offset] |
	    (uint16_t)txn->sm->sm[txn->sm->ip_bitmap_offset + 1] << 8);
	if (slot >= txn->sm->ip_bm_block_count) {
		return EIO;
	}
	error = apfs_block_read_raw(txn->nx, txn->sm->ip_bm_base + slot, &bitmap);
	if (error) {
		return EIO;
	}
	txn->ipbm = apfs_alloc(txn_bs(txn));
	if (txn->ipbm == NULL) {
		return ENOMEM;
	}
	memcpy(txn->ipbm, bitmap, txn_bs(txn));
	txn->ip_dirty = false;
	return 0;
}

static int
txn_ip_alloc(struct apfs_txn *txn, uint32_t n, uint64_t *out)
{
	uint32_t got = 0, i;
	int error;

	error = txn_ip_prepare(txn);
	if (error) {
		return error;
	}
	for (i = 0; i < txn->sm->ip_block_count && got < n; i++) {
		if (((txn->ipbm[i >> 3] >> (i & 7)) & 1) != 0) {
			continue;
		}
		txn->ipbm[i >> 3] |= (uint8_t)(1 << (i & 7));
		txn->ip_dirty = true;
		out[got++] = txn->sm->ip_base + i;
	}
	if (got != n) {
		apfs_log("the internal pool has no room for %u blocks\n", n);
		return ENOSPC;
	}
	return 0;
}

static int
txn_ip_free(struct apfs_txn *txn, uint64_t addr)
{
	if (addr < txn->sm->ip_base || addr >= txn->sm->ip_base + txn->sm->ip_block_count) {
		apfs_log("block %llu is not in the internal pool\n", (unsigned long long)addr);
		return EIO;
	}
	return txn_queue_simple(&txn->freed_ip, &txn->nfreed_ip, &txn->cfreed_ip, addr);
}

/* The pool's blocks that were given up, once nothing more is to be taken from it. */
static int
txn_apply_ip_frees(struct apfs_txn *txn)
{
	uint32_t i;

	for (i = 0; i < txn->nfreed_ip; i++) {
		uint64_t at = txn->freed_ip[i] - txn->sm->ip_base;

		txn->ipbm[at >> 3] &= (uint8_t)~(1 << (at & 7));
		txn->ip_dirty = true;
	}
	return 0;
}

/* ------------------------------------------------------------ finishing: the space manager */

/*
 * The chunk bitmaps and chunk-info blocks that changed, copied inside the internal pool: a
 * fresh bitmap block per chunk, a fresh chunk-info block per block that holds one of them,
 * the old ones given back to the pool.  The new addresses go into the space manager's address
 * array, which the copy of it in the data ring is written from.
 */
static int
txn_finish_spaceman(struct apfs_txn *txn)
{
	struct apfs_spaceman *sm = txn->sm;
	uint32_t bs = txn_bs(txn);
	uint32_t i, j, cib;
	int error;

	if (sm->cab_count != 0) {
		apfs_log("the space manager has chunk-info address blocks; not written here\n");
		return ENOTSUP;
	}
	error = txn_apply_frees(txn);
	if (error) {
		return error;
	}

	/* a fresh bitmap block per dirty chunk, and how many of its blocks are free */
	for (i = 0; i < txn->nchunks; i++) {
		struct txn_chunk *c = &txn->chunks[i];
		struct apfs_chunk_info ci;
		uint8_t *bm;
		uint32_t bytes, nfree = 0;

		if (!c->dirty) {
			continue;
		}
		error = txn_ip_alloc(txn, 1, &c->new_bm);
		if (error) {
			return error;
		}
		if (apfs_spaceman_chunk(sm, c->index, &ci) != 0) {
			return EIO;
		}
		bm = apfs_alloc(bs);
		if (bm == NULL) {
			return ENOMEM;
		}
		memcpy(bm, c->bm, bs);
		bytes = (ci.ci_block_count + 7) / 8;
		if (bytes < bs) {
			memset(bm + bytes, 0, bs - bytes);
		}
		if ((ci.ci_block_count & 7) != 0 && bytes != 0) {
			bm[bytes - 1] &= (uint8_t)((1u << (ci.ci_block_count & 7)) - 1);
		}
		for (j = 0; j < ci.ci_block_count; j++) {
			if (((bm[j >> 3] >> (j & 7)) & 1) == 0) {
				nfree++;
			}
		}
		c->free = nfree;
		error = txn_stage(txn, c->new_bm, bm);
		apfs_free(bm, bs);
		if (error) {
			return error;
		}
	}

	/* one fresh chunk-info block per block that holds a changed chunk */
	for (cib = 0; cib < sm->cib_count; cib++) {
		uint8_t *cib_block;
		uint64_t old_cib, new_cib;
		uint32_t at, count, patched = 0;

		for (i = 0; i < txn->nchunks; i++) {
			if (txn->chunks[i].dirty && txn->chunks[i].index / sm->chunks_per_cib == cib) {
				patched++;
			}
		}
		if (patched == 0) {
			continue;
		}
		old_cib = t_le64(sm->sm + sm->addr_offset + (size_t)cib * 8);
		if (old_cib == 0) {
			return EIO;
		}
		if (txn_copy_block(txn, old_cib, &cib_block) != 0) {
			return EIO;
		}
		if ((t_le32(cib_block + 24) & APFS_OBJECT_TYPE_MASK) != APFS_OBJECT_TYPE_SPACEMAN_CIB) {
			apfs_free(cib_block, bs);
			return EIO;
		}
		count = t_le32(cib_block + 36);
		for (i = 0; i < txn->nchunks; i++) {
			struct txn_chunk *c = &txn->chunks[i];
			struct apfs_chunk_info ci;
			uint8_t *e;

			if (!c->dirty || c->index / sm->chunks_per_cib != cib) {
				continue;
			}
			if (apfs_spaceman_chunk(sm, c->index, &ci) != 0) {
				apfs_free(cib_block, bs);
				return EIO;
			}
			at = c->index % sm->chunks_per_cib;
			if (at >= count || 40 + (size_t)at * 32 + 32 > bs) {
				apfs_free(cib_block, bs);
				return EIO;
			}
			e = cib_block + 40 + (size_t)at * 32;
			t_put64(e, txn->xid);                    /* ci_xid */
			t_put64(e + 8, (uint64_t)ci.ci_addr);     /* ci_addr, unchanged */
			t_put32(e + 16, ci.ci_block_count);
			t_put32(e + 20, c->free);
			t_put64(e + 24, c->new_bm);
		}
		error = txn_ip_alloc(txn, 1, &new_cib);
		if (error) {
			apfs_free(cib_block, bs);
			return error;
		}
		txn_seal(cib_block, bs, new_cib, txn->xid, APFS_OBJ_PHYSICAL |
		    APFS_OBJECT_TYPE_SPACEMAN_CIB, 0);
		error = txn_stage(txn, new_cib, cib_block);
		apfs_free(cib_block, bs);
		if (error) {
			return error;
		}
		if (txn_ip_free(txn, old_cib) != 0) {
			return EIO;
		}
		t_put64(sm->sm + sm->addr_offset + (size_t)cib * 8, new_cib);
		for (i = 0; i < txn->nchunks; i++) {
			struct txn_chunk *c = &txn->chunks[i];

			if (c->dirty && c->index / sm->chunks_per_cib == cib && c->old_bm != 0) {
				if (txn_ip_free(txn, c->old_bm) != 0) {
					return EIO;
				}
			}
		}
	}

	/* and the pool's own blocks that were given up, now that nothing more is taken */
	if (txn_apply_ip_frees(txn) != 0) {
		return EIO;
	}

	/* the internal pool's own bitmap: the live copy moves to the next free slot of the ring.
	 * This must come after txn_apply_ip_frees: the staged copy is what the next transaction
	 * reads, so it has to show the blocks this one gave back as free. */
	if (txn->ip_dirty) {
		uint32_t slot = sm->ip_bm_free_head;
		uint32_t tail = sm->ip_bm_free_tail;
		uint32_t next, live;
		uint8_t *p;

		if (slot >= sm->ip_bm_block_count || tail >= sm->ip_bm_block_count) {
			apfs_log("the internal pool's bitmap ring has no free slot\n");
			return ENOSPC;
		}
		live = (uint32_t)(sm->sm[sm->ip_bitmap_offset] |
		    (uint16_t)sm->sm[sm->ip_bitmap_offset + 1] << 8);
		if (live >= sm->ip_bm_block_count) {
			return EIO;
		}
		p = sm->sm + sm->ip_bm_free_next_offset;
		next = (uint32_t)(p[slot * 2] | (uint16_t)p[slot * 2 + 1] << 8);
		p[slot * 2] = (uint8_t)(APFS_SPACEMAN_IP_BM_INDEX_INVALID & 0xff);
		p[slot * 2 + 1] = (uint8_t)(APFS_SPACEMAN_IP_BM_INDEX_INVALID >> 8);
		p[tail * 2] = (uint8_t)live;
		p[tail * 2 + 1] = (uint8_t)(live >> 8);
		p[live * 2] = (uint8_t)(APFS_SPACEMAN_IP_BM_INDEX_INVALID & 0xff);
		p[live * 2 + 1] = (uint8_t)(APFS_SPACEMAN_IP_BM_INDEX_INVALID >> 8);
		sm->sm[sm->ip_bitmap_offset] = (uint8_t)slot;
		sm->sm[sm->ip_bitmap_offset + 1] = (uint8_t)(slot >> 8);
		t_put16(sm->sm + offsetof(struct apfs_spaceman_phys, sm_ip_bm_free_head), (uint16_t)next);
		t_put16(sm->sm + offsetof(struct apfs_spaceman_phys, sm_ip_bm_free_tail), (uint16_t)live);
		t_put64(sm->sm + sm->ip_bm_xid_offset, txn->xid);
		error = txn_stage(txn, sm->ip_bm_base + slot, txn->ipbm);
		if (error) {
			return error;
		}
	}
	/* what the container has free now, in the device's own record */
	t_put64(sm->sm + offsetof(struct apfs_spaceman_phys, sm_dev) +
	    offsetof(struct apfs_spaceman_device, sm_free_count), sm->free_count);
	return 0;
}

/* --------------------------------------------------------------------- the commit */

/*
 * All of it goes out in the order it was staged -- the structures a reader would follow
 * first, the commit last.  The blocks that lie next to each other in the order staged go out
 * as one request, up to APFS_WRITE_GROUP of them: one transfer of the loader's block service
 * costs some 400 us whatever its length, so a snapshot of 3 MB was 750 transfers, and a
 * transaction of a commit's blocks held the writer's mutex for a second and more (p393:
 * SpringBoard waited 0.3 to 1.1 s for logd, assetsd and SpringBoard itself).  The read back
 * and compare is one per group as well, and boot-arg apfs_verify=0 leaves it out (the loader
 * does not verify its own writes either).
 */
#define APFS_WRITE_GROUP    32          /* blocks in one request: 128 KB */

static int
txn_write_all(struct apfs_txn *txn)
{
	struct apfs_nx *nx = txn->nx;
	uint32_t bs = nx->block_size;
	uint8_t *group = apfs_alloc((size_t)APFS_WRITE_GROUP * bs);
	uint32_t i;
	int error = 0;

	if (group == NULL) {
		return ENOMEM;
	}
	for (i = 0; i < txn->npend && error == 0;) {
		uint64_t at = txn->pend[i].addr * (uint64_t)bs;
		uint32_t n = 1, k;

		while (i + n < txn->npend && n < APFS_WRITE_GROUP &&
		    txn->pend[i + n].addr == txn->pend[i].addr + n) {
			n++;
		}
		if (n == 1) {
			error = nx->dev.write(nx->dev.ctx, at, txn->pend[i].data, bs);
		} else {
			for (k = 0; k < n; k++) {
				memcpy(group + (size_t)k * bs, txn->pend[i + k].data, bs);
			}
			error = nx->dev.write(nx->dev.ctx, at, group, (size_t)n * bs);
		}
		if (error) {
			apfs_log("block %llu: the device refused the write: %d\n",
			    (unsigned long long)txn->pend[i].addr, error);
			break;
		}
		if (nx->write_verify) {
			uint8_t *where = apfs_alloc((size_t)n * bs);

			if (where == NULL) {
				error = ENOMEM;
				break;
			}
			if (nx->dev.read_data != NULL) {
				error = nx->dev.read_data(nx->dev.ctx, at, where, (size_t)n * bs);
			} else {
				error = nx->dev.read(nx->dev.ctx, at, where, (size_t)n * bs);
			}
			if (error == 0 &&
			    memcmp(where, n == 1 ? txn->pend[i].data : group, (size_t)n * bs) != 0) {
				apfs_log("block %llu read back as something else than it was written\n",
				    (unsigned long long)txn->pend[i].addr);
				error = EIO;
			}
			apfs_free(where, (size_t)n * bs);
			if (error) {
				break;
			}
		}
		i += n;
	}
	apfs_free(group, (size_t)APFS_WRITE_GROUP * bs);
	return error;
}

/*
 * The ephemeral objects of the checkpoint, copied into the data ring in the order the map
 * names them and with no gap: the space manager is rewritten, the others (the reaper, the two
 * free queues) are copies with the new transaction's header.
 */
static int
txn_ephemerals(struct apfs_txn *txn, uint64_t *new_at, uint64_t *data_at)
{
	struct apfs_nx *nx = txn->nx;
	struct apfs_spaceman *sm = txn->sm;
	uint32_t bs = nx->block_size;
	uint32_t m;

	*data_at = nx->xp_data_next % nx->xp_data_blocks;
	if (*data_at + 4 > nx->xp_data_blocks) {
		*data_at = 0;
	}
	for (m = 0; m < sm->map_count; m++) {
		struct apfs_sm_map *mp = &sm->map[m];
		uint64_t blocks = (mp->size + bs - 1) / bs;
		uint64_t at = nx->xp_data_base + *data_at;
		uint8_t *obj;
		uint32_t i;
		int error;

		if (blocks == 0 || *data_at + blocks > nx->xp_data_blocks ||
		    mp->size > 4 * bs) {
			apfs_log("ephemeral object %u does not fit the data ring\n", m);
			return ENOSPC;
		}
		if ((mp->type & APFS_OBJECT_TYPE_MASK) == APFS_OBJECT_TYPE_SPACEMAN) {
			obj = sm->sm;
			if (mp->size != sm->sm_size) {
				return EIO;
			}
		} else {
			obj = apfs_alloc(blocks * bs);
			if (obj == NULL) {
				return ENOMEM;
			}
			for (i = 0; i < blocks; i++) {
				const uint8_t *blk;

				if (apfs_block_read(nx, mp->paddr + i, &blk) != 0) {
					apfs_free(obj, blocks * bs);
					return EIO;
				}
				memcpy(obj + (size_t)i * bs, blk, bs);
			}
		}
		txn_seal(obj, mp->size, mp->oid, txn->xid, mp->type, mp->subtype);
		for (i = 0; i < blocks; i++) {
			error = txn_stage(txn, at + i, obj + (size_t)i * bs);
			if (error) {
				if (obj != sm->sm) {
					apfs_free(obj, blocks * bs);
				}
				return error;
			}
		}
		if (obj != sm->sm) {
			apfs_free(obj, blocks * bs);
		}
		new_at[m] = at;
		*data_at += blocks;
	}
	return 0;
}

int
apfs_txn_commit(struct apfs_txn *txn)
{
	struct apfs_nx *nx = txn->nx;
	struct apfs_spaceman *sm = txn->sm;
	uint32_t bs = nx->block_size;
	uint64_t new_at[APFS_SM_MAX_MAPS];
	uint64_t data_at = 0, data_index, desc_at;
	uint32_t m;
	uint8_t *blk;
	int error;

	memset(new_at, 0, sizeof(new_at));
	if (sm->map_count == 0 || sm->map_count > APFS_SM_MAX_MAPS) {
		return EIO;
	}
	if (nx->omap_addr == 0) {
		apfs_log("the transaction does not say where the container's object map went\n");
		return EIO;
	}
	error = txn_finish_spaceman(txn);
	if (error) {
		return error;
	}
	error = txn_ephemerals(txn, new_at, &data_at);
	if (error) {
		return error;
	}
	if (nx->xp_desc_blocks < 2) {
		apfs_log("the checkpoint descriptor ring holds %u blocks; two are needed\n",
		    nx->xp_desc_blocks);
		return ENOTSUP;
	}

	/* the checkpoint map: one block, the last of its checkpoint, naming the new copies */
	desc_at = nx->xp_desc_next % nx->xp_desc_blocks;
	blk = apfs_alloc(bs);
	if (blk == NULL) {
		return ENOMEM;
	}
	memset(blk, 0, bs);
	t_put32(blk + 32, APFS_CHECKPOINT_MAP_LAST);
	t_put32(blk + 36, sm->map_count);
	for (m = 0; m < sm->map_count; m++) {
		uint8_t *e = blk + 40 + (size_t)m * 40;

		t_put32(e, sm->map[m].type);
		t_put32(e + 4, sm->map[m].subtype);
		t_put32(e + 8, sm->map[m].size);
		t_put64(e + 24, sm->map[m].oid);
		t_put64(e + 32, new_at[m]);
	}
	txn_seal(blk, bs, nx->xp_desc_base + desc_at, txn->xid, APFS_OBJ_PHYSICAL |
	    APFS_OBJECT_TYPE_CHECKPOINT_MAP, 0);
	error = txn_stage(txn, nx->xp_desc_base + desc_at, blk);
	apfs_free(blk, bs);
	if (error) {
		return error;
	}

	/* the commit: the container superblock, one transaction ahead */
	{
		uint8_t *sb = apfs_alloc(bs);

		if (sb == NULL) {
			return ENOMEM;
		}
		data_index = nx->xp_data_next % nx->xp_data_blocks;
		if (data_index + 4 > nx->xp_data_blocks) {
			data_index = 0;
		}
		memcpy(sb, nx->sb, bs);
		t_put64(sb + 96, txn->xid + 1);                 /* nx_next_xid */
		t_put64(sb + 160, nx->omap_addr);               /* nx_omap_oid, already the new one */
		t_put32(sb + 128, (uint32_t)((desc_at + 2) % nx->xp_desc_blocks));
		t_put32(sb + 132, (uint32_t)(data_at % nx->xp_data_blocks));
		t_put32(sb + 136, (uint32_t)desc_at);
		t_put32(sb + 140, 2);
		t_put32(sb + 144, (uint32_t)data_index);
		t_put32(sb + 148, (uint32_t)(data_at - data_index));
		txn_seal(sb, bs, APFS_OID_NX_SUPERBLOCK, txn->xid, APFS_OBJ_EPHEMERAL |
		    APFS_OBJECT_TYPE_NX_SUPERBLOCK, 0);
		error = txn_stage(txn, nx->xp_desc_base +
		    (desc_at + 1) % nx->xp_desc_blocks, sb);
		if (error) {
			apfs_free(sb, bs);
			return error;
		}
		/* block zero, the copy a clean unmount leaves: not part of the commit */
		error = txn_stage(txn, 0, sb);
		apfs_free(sb, bs);
		if (error) {
			return error;
		}
	}

	/* out they go */
	error = txn_write_all(txn);
	if (error) {
		return error;
	}

	/*
	 * Every block the transaction wrote is now different on the device: take the old
	 * copies out of the read cache, or the next reader gets the checkpoint before this one.
	 */
	for (uint32_t i = 0; i < txn->npend; i++) {
		apfs_nx_cache_drop(nx, txn->pend[i].addr);
	}

	/* what the container now is, for whoever goes on using this mount */
	nx->xid = txn->xid;
	nx->xp_desc_next = (uint32_t)((desc_at + 2) % nx->xp_desc_blocks);
	nx->xp_desc_index = (uint32_t)desc_at;
	nx->xp_desc_len = 2;
	nx->xp_data_next = (uint32_t)(data_at % nx->xp_data_blocks);
	memcpy(nx->sb, txn->pend[txn->npend - 2].data, bs);      /* the commit copy */
	return 0;
}

/* ------------------------------------------------------------------- tree nodes */

/*
 * One record of a leaf: where its key and its value are in the node, and how long each is.
 * Both shapes of table of contents are here -- the file-system tree's entries are of fixed
 * size but the keys and values vary, an object map's are fixed in both.
 */
struct txn_rec {
	uint32_t key_at, val_at;
	uint16_t key_len, val_len;
};

static int
node_rec(const uint8_t *node, uint32_t bs, uint32_t index, struct txn_rec *rec)
{
	uint16_t flags = (uint16_t)(node[32] | (uint16_t)node[33] << 8);
	uint32_t nkeys = t_le32(node + 36);
	uint32_t toc_off = (uint32_t)(node[40] | (uint16_t)node[41] << 8);
	uint32_t toc_len = (uint32_t)(node[42] | (uint16_t)node[43] << 8);
	uint32_t toc, key_base, val_end;
	bool root = (flags & TXN_BTNODE_ROOT) != 0;

	if ((flags & TXN_BTNODE_LEAF) == 0 || index >= nkeys) {
		return ENOENT;
	}
	toc = 56 + toc_off;
	key_base = toc + toc_len;
	val_end = root ? bs - 40 : bs;
	if (toc < 56 || toc > bs || key_base > val_end) {
		return EIO;
	}
	if ((flags & TXN_BTNODE_FIXED_KV) != 0) {
		uint32_t kl, vl;
		uint32_t koff, voff;

		if (!root || bs < 56) {
			return EIO;
		}
		kl = t_le32(node + bs - 40 + 8);
		vl = t_le32(node + bs - 40 + 12);
		if (toc + 4 * (size_t)(index + 1) > key_base) {
			return EIO;
		}
		koff = (uint32_t)(node[toc + 4 * index] | (uint16_t)node[toc + 4 * index + 1] << 8);
		voff = (uint32_t)(node[toc + 4 * index + 2] | (uint16_t)node[toc + 4 * index + 3] << 8);
		if (key_base + koff + kl > val_end || (uint32_t)voff + vl > val_end) {
			return EIO;
		}
		rec->key_at = key_base + koff;
		rec->key_len = (uint16_t)kl;
		rec->val_at = val_end - voff;
		rec->val_len = (uint16_t)vl;
		return 0;
	} else {
		uint32_t koff, voff, klen, vlen;

		if (toc + 8 * (size_t)(index + 1) > key_base) {
			return EIO;
		}
		koff = (uint32_t)(node[toc + 8 * index] | (uint16_t)node[toc + 8 * index + 1] << 8);
		klen = (uint32_t)(node[toc + 8 * index + 2] | (uint16_t)node[toc + 8 * index + 3] << 8);
		voff = (uint32_t)(node[toc + 8 * index + 4] | (uint16_t)node[toc + 8 * index + 5] << 8);
		vlen = (uint32_t)(node[toc + 8 * index + 6] | (uint16_t)node[toc + 8 * index + 7] << 8);
		if (key_base + koff + klen > val_end || (uint32_t)voff + vlen > val_end) {
			return EIO;
		}
		rec->key_at = key_base + koff;
		rec->key_len = (uint16_t)klen;
		rec->val_at = val_end - voff;
		rec->val_len = (uint16_t)vlen;
		return 0;
	}
}

/* The record whose key is this 8-byte one, in a leaf that is the whole tree. */
static int
node_find_key(const uint8_t *node, uint32_t bs, uint64_t key, struct txn_rec *rec)
{
	uint32_t i;
	struct txn_rec r;
	int error;

	for (i = 0; ; i++) {
		error = node_rec(node, bs, i, &r);
		if (error == ENOENT) {
			return ENOENT;
		}
		if (error) {
			return error;
		}
		if (r.key_len == 8 && t_le64(node + r.key_at) == key) {
			*rec = r;
			return 0;
		}
	}
}

/*
 * An entry of an object map: the key is (identifier, transaction) and the value carries the
 * block address.  The entry to change is the newest one for that identifier; with no snapshot
 * in the picture there is one.
 */
static int
node_omap_patch(uint8_t *node, uint32_t bs, apfs_oid_t oid, apfs_xid_t xid, uint64_t paddr)
{
	struct txn_rec r;
	uint32_t i;
	int patched = 0;
	apfs_xid_t best = 0;

	for (i = 0; ; i++) {
		int error = node_rec(node, bs, i, &r);

		if (error == ENOENT) {
			break;
		}
		if (error) {
			return -error;
		}
		if (r.key_len < 16 || r.val_len < 16 || t_le64(node + r.key_at) != oid) {
			continue;
		}
		if (t_le64(node + r.key_at + 8) >= best) {
			best = t_le64(node + r.key_at + 8);
			t_put64(node + r.key_at + 8, xid);
			t_put64(node + r.val_at + 8, paddr);
			patched = 1;
		}
	}
	return patched;
}

/* ------------------------------------------------------- one change of metadata */

/*
 * The modification time of one inode: the smallest change that is still a change of metadata.
 * The tree has to be a single node, which is the shape every image this is tried on has; the
 * rest of what a real create or unlink needs -- splitting a leaf, merging one, inserting and
 * deleting records -- is not here yet.
 */
int
apfs_inode_set_mod_time(struct apfs_vol *vol, uint64_t obj_id, uint64_t when)
{
	struct apfs_nx *nx = vol->nx;
	struct apfs_spaceman sm;
	struct apfs_txn *txn = NULL;
	uint8_t *fs = NULL, *votree = NULL, *vo = NULL, *vsb = NULL, *nxtree = NULL, *nxo = NULL;
	uint64_t newb[6];
	uint64_t key = ((uint64_t)APFS_TYPE_INODE << APFS_OBJ_TYPE_SHIFT) | obj_id;
	uint32_t bs = nx->block_size;
	struct txn_rec rec;
	int error;

	if (nx->dev.write == NULL) {
		return EROFS;
	}
	if ((vol->incompat & APFS_INCOMPAT_SEALED_VOLUME) != 0 ||
	    vol->integrity_meta_oid != 0 || vol->num_snapshots != 0) {
		return EROFS;
	}
	if (vol->tree.node_count != 1 || vol->omap.tree.node_count != 1 ||
	    nx->omap.tree.node_count != 1) {
		apfs_log("a tree of %llu/%llu/%llu nodes: this changes a tree of one\n",
		    (unsigned long long)vol->tree.node_count,
		    (unsigned long long)vol->omap.tree.node_count,
		    (unsigned long long)nx->omap.tree.node_count);
		return ENOTSUP;
	}
	error = apfs_spaceman_open(nx, &sm);
	if (error) {
		return error;
	}
	txn = apfs_txn_begin(nx, &sm);
	if (txn == NULL) {
		apfs_spaceman_close(&sm);
		return EIO;
	}

	/* every fresh block first: nothing is given back until all six are in hand */
	error = txn_alloc(txn, 6, newb);
	if (error) {
		goto out;
	}

	/* 1. the file-system tree's only node, with the one value changed */
	error = txn_copy_block(txn, vol->tree.root, &fs);
	if (error) {
		goto out;
	}
	error = node_find_key(fs, bs, key, &rec);
	if (error) {
		apfs_log("inode %llu: no record in the one node of the tree\n",
		    (unsigned long long)obj_id);
		goto out;
	}
	if (rec.val_len < (uint16_t)(offsetof(struct apfs_j_inode_val, mod_time) + 8)) {
		error = EIO;
		goto out;
	}
	t_put64(fs + rec.val_at + offsetof(struct apfs_j_inode_val, mod_time), when);
	txn_seal(fs, bs, vol->tree.root_oid, txn->xid, APFS_OBJ_VIRTUAL | APFS_OBJECT_TYPE_BTREE,
	    APFS_OBJECT_TYPE_FSTREE);
	error = txn_stage(txn, newb[0], fs);
	if (error) {
		goto out;
	}

	/* 2. the volume's object map: its entry for that node now points at the new copy */
	error = txn_copy_block(txn, vol->omap.tree.root, &votree);
	if (error) {
		goto out;
	}
	if (node_omap_patch(votree, bs, vol->tree.root_oid, txn->xid, newb[0]) != 1) {
		apfs_log("the volume's object map has no one entry for the tree's root\n");
		error = EIO;
		goto out;
	}
	txn_seal(votree, bs, newb[1], txn->xid, APFS_OBJ_PHYSICAL | APFS_OBJECT_TYPE_BTREE,
	    APFS_OBJECT_TYPE_OMAP);
	error = txn_stage(txn, newb[1], votree);
	if (error) {
		goto out;
	}

	/* 3. the volume's object map object: the tree it names */
	error = txn_copy_block(txn, vol->omap_addr, &vo);
	if (error) {
		goto out;
	}
	t_put64(vo + 48, newb[1]);                      /* om_tree_oid */
	txn_seal(vo, bs, newb[2], txn->xid, APFS_OBJ_PHYSICAL | APFS_OBJECT_TYPE_OMAP, 0);
	error = txn_stage(txn, newb[2], vo);
	if (error) {
		goto out;
	}

	/* 4. the volume superblock: the map it names */
	error = txn_copy_block(txn, vol->sb_addr, &vsb);
	if (error) {
		goto out;
	}
	t_put64(vsb + 128, newb[2]);                    /* apfs_omap_oid */
	txn_seal(vsb, bs, vol->oid, txn->xid, APFS_OBJ_VIRTUAL | APFS_OBJECT_TYPE_FS, 0);
	error = txn_stage(txn, newb[3], vsb);
	if (error) {
		goto out;
	}

	/* 5. the container's object map: its entry for the volume superblock */
	error = txn_copy_block(txn, nx->omap.tree.root, &nxtree);
	if (error) {
		goto out;
	}
	if (node_omap_patch(nxtree, bs, vol->oid, txn->xid, newb[3]) != 1) {
		apfs_log("the container's object map has no one entry for the volume\n");
		error = EIO;
		goto out;
	}
	txn_seal(nxtree, bs, newb[4], txn->xid, APFS_OBJ_PHYSICAL | APFS_OBJECT_TYPE_BTREE,
	    APFS_OBJECT_TYPE_OMAP);
	error = txn_stage(txn, newb[4], nxtree);
	if (error) {
		goto out;
	}

	/* 6. the container's object map object */
	error = txn_copy_block(txn, nx->omap_addr, &nxo);
	if (error) {
		goto out;
	}
	t_put64(nxo + 48, newb[4]);
	txn_seal(nxo, bs, newb[5], txn->xid, APFS_OBJ_PHYSICAL | APFS_OBJECT_TYPE_OMAP, 0);
	error = txn_stage(txn, newb[5], nxo);
	if (error) {
		goto out;
	}

	/* what the old checkpoint used is given back, once the new blocks are all in hand */
	if (apfs_txn_free_block_who(txn, vol->tree.root, APFS_FREE_COMMIT, 0) != 0 ||
	    apfs_txn_free_block_who(txn, vol->omap.tree.root, APFS_FREE_COMMIT, 1) != 0 ||
	    apfs_txn_free_block_who(txn, vol->omap_addr, APFS_FREE_COMMIT, 2) != 0 ||
	    apfs_txn_free_block_who(txn, vol->sb_addr, APFS_FREE_COMMIT, 3) != 0 ||
	    apfs_txn_free_block_who(txn, nx->omap.tree.root, APFS_FREE_COMMIT, 4) != 0 ||
	    apfs_txn_free_block_who(txn, nx->omap_addr, APFS_FREE_COMMIT, 5) != 0) {
		error = EIO;
		goto out;
	}

	/* the container's superblock names the new map object; the commit does the rest */
	uint64_t old_nx_omap = nx->omap_addr;
	nx->omap_addr = newb[5];
	error = apfs_txn_commit(txn);
	if (error) {
		nx->omap_addr = old_nx_omap;
		goto out;
	}

	/* and the mount goes on from the new copies */
	vol->tree.root = newb[0];
	vol->omap.tree.root = newb[1];
	vol->omap_addr = newb[2];
	vol->sb_addr = newb[3];
	nx->omap.tree.root = newb[4];
	apfs_free(fs, bs);
	apfs_free(votree, bs);
	apfs_free(vo, bs);
	apfs_free(vsb, bs);
	apfs_free(nxtree, bs);
	apfs_free(nxo, bs);
	apfs_txn_abort(txn);
	apfs_spaceman_close(&sm);
	return 0;
out:
	apfs_free(fs, bs);
	apfs_free(votree, bs);
	apfs_free(vo, bs);
	apfs_free(vsb, bs);
	apfs_free(nxtree, bs);
	apfs_free(nxo, bs);
	apfs_txn_abort(txn);
	apfs_spaceman_close(&sm);
	return error;
}


/* --------------------------------------------------------- the seam for the tree writer */

int
apfs_txn_alloc_block(struct apfs_txn *txn, uint64_t *out)
{
	return txn_alloc(txn, 1, out);
}

int
apfs_txn_free_block(struct apfs_txn *txn, uint64_t addr)
{
	return txn_free(txn, addr, APFS_FREE_OTHER, 0);
}

int
apfs_txn_free_block_who(struct apfs_txn *txn, uint64_t addr, uint8_t who, uint64_t arg)
{
	return txn_free(txn, addr, who, arg);
}

int
apfs_txn_stage_block(struct apfs_txn *txn, uint64_t addr, const uint8_t *data)
{
	return txn_stage(txn, addr, data);
}

int
apfs_txn_copy_block(struct apfs_txn *txn, uint64_t addr, uint8_t **out)
{
	return txn_copy_block(txn, addr, out);
}

int
apfs_txn_read_block(struct apfs_txn *txn, uint64_t addr, uint8_t **out)
{
	uint32_t i;

	*out = NULL;
	for (i = txn->npend; i > 0; i--) {
		if (txn->pend[i - 1].addr == addr) {
			*out = apfs_alloc(txn_bs(txn));
			if (*out == NULL) {
				return ENOMEM;
			}
			memcpy(*out, txn->pend[i - 1].data, txn_bs(txn));
			return 0;
		}
	}
	return txn_copy_block(txn, addr, out);
}

struct apfs_spaceman *
apfs_txn_spaceman(struct apfs_txn *txn)
{
	return txn->sm;
}

apfs_xid_t
apfs_txn_xid(struct apfs_txn *txn)
{
	return txn->xid;
}

uint32_t
apfs_txn_block_size(struct apfs_txn *txn)
{
	return txn_bs(txn);
}

void
apfs_txn_seal(uint8_t *blk, uint32_t size, apfs_oid_t oid, apfs_xid_t xid, uint32_t type,
    uint32_t subtype)
{
	txn_seal(blk, size, oid, xid, type, subtype);
}
