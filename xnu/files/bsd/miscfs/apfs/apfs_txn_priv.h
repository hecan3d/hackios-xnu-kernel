/*
 * The parts of a transaction that the tree writer uses.  Not for anything outside
 * bsd/miscfs/apfs: apfs_lib.h is the public side, this is the seam between apfs_txn.c,
 * apfs_btree_write.c and the volume operations in apfs_txn.c.
 */

#ifndef _MISCFS_APFS_APFS_TXN_PRIV_H_
#define _MISCFS_APFS_APFS_TXN_PRIV_H_

#include "apfs_lib.h"

struct apfs_txn;

/*
 * Who gives a block back: not for the transaction, but for the log line that says a block
 * went back twice.  A number that means nothing is worse than a name.
 */
#define APFS_FREE_OTHER     0
#define APFS_FREE_NODE      1       /* arg: the node's oid */
#define APFS_FREE_EXTENT    2       /* arg: the stream id (inode private id) */
#define APFS_FREE_TRUNC     3       /* arg: the stream id, the tail of the last extent */
#define APFS_FREE_INODE     4       /* arg: the inode number, its extents on the way out */
#define APFS_FREE_COMMIT    5       /* arg: which object of the commit it retires (0..5) */

const char *apfs_free_who_name(uint8_t who);

/* one block, out of the container's free space */
int  apfs_txn_alloc_block(struct apfs_txn *txn, uint64_t *out);
int  apfs_txn_free_block(struct apfs_txn *txn, uint64_t addr);
int  apfs_txn_free_block_who(struct apfs_txn *txn, uint64_t addr, uint8_t who, uint64_t arg);

/* one block, queued to be written when the transaction commits, in this order */
int  apfs_txn_stage_block(struct apfs_txn *txn, uint64_t addr, const uint8_t *data);

/* a copy of a block, for changing */
int  apfs_txn_copy_block(struct apfs_txn *txn, uint64_t addr, uint8_t **out);

/*
 * A copy of a block as the transaction has left it: a block this transaction has already
 * written out (which is still only in memory, see the commit) comes from there, anything else
 * from the device.  A tree changed twice in one transaction has to read its own new nodes.
 */
int  apfs_txn_read_block(struct apfs_txn *txn, uint64_t addr, uint8_t **out);

struct apfs_spaceman *apfs_txn_spaceman(struct apfs_txn *txn);
apfs_xid_t apfs_txn_xid(struct apfs_txn *txn);
uint32_t   apfs_txn_block_size(struct apfs_txn *txn);

/* the object header and the checksum, as every object has them */
void apfs_txn_seal(uint8_t *blk, uint32_t size, apfs_oid_t oid, apfs_xid_t xid, uint32_t type,
    uint32_t subtype);

/* ---- a B-tree with a change made to it (apfs_btree_write.c) ---- */

struct btp_tree;

/* The state of a tree being changed: the shape it has, and what the last change made.  The
 * caller has one of these on its stack; nothing outside apfs_btree_write.c looks inside. */
struct btp_map_change {
	apfs_oid_t oid;         /* the object whose record this is */
	uint64_t   addr;
	apfs_oid_t old_oid;     /* the record this one retires, if any */
	apfs_xid_t old_xid;
};

struct btp_tree {
	struct apfs_txn *txn;
	uint32_t         bs;
	bool             fixed;
	bool             physical;
	uint32_t         key_size;
	uint32_t         val_size;
	uint32_t         tree_flags;
	apfs_oid_t       root_oid;
	uint64_t         next_oid;
	uint32_t         subtype;
	int            (*cmp)(const uint8_t *a, uint16_t alen, const uint8_t *b, uint16_t blen);
	int            (*resolve)(void *ctx, apfs_oid_t oid, uint64_t *addr,
	                              apfs_xid_t *key_xid);
	void            *resolve_ctx;
	apfs_xid_t       root_xid;      /* the record the root's identifier names now */
	uint64_t         new_root;
	apfs_oid_t       new_root_oid;
	uint64_t         key_count;
	uint64_t         node_count;
	uint64_t         removed, added;
	uint32_t         longest_key, longest_val;
	struct btp_map_change *changes;
	uint32_t         nchanges, cchanges;
};

#define BTP_INSERT  1
#define BTP_DELETE  2
#define BTP_REPLACE 3

int  btp_open(struct btp_tree *t, struct apfs_txn *txn, uint64_t root, apfs_oid_t root_oid,
    bool fixed, uint32_t key_size, uint32_t val_size, uint32_t tree_flags, uint32_t subtype,
    int (*cmp)(const uint8_t *, uint16_t, const uint8_t *, uint16_t),
    int (*resolve)(void *ctx, apfs_oid_t oid, uint64_t *addr, apfs_xid_t *key_xid),
    void *resolve_ctx, uint64_t next_oid);
void btp_close(struct btp_tree *t);
int  btp_update(struct btp_tree *t, int op, const uint8_t *key, uint16_t klen,
    const uint8_t *val, uint16_t vlen);

/* what the tree is now, for whoever writes the superblocks: the root's block address, the
 * identifier it kept, the record and node counts, and the next free identifier. */
uint64_t   btp_next_oid(struct btp_tree *t);
void       btp_set_next_oid(struct btp_tree *t, uint64_t next);
uint64_t   btp_key_count(struct btp_tree *t);
uint64_t   btp_node_count(struct btp_tree *t);

/* the new nodes of a virtual tree: the object map has to be told about every one */
uint32_t btp_change_count(struct btp_tree *t);
int      btp_change_at(struct btp_tree *t, uint32_t i, apfs_oid_t *oid, uint64_t *addr);
int      btp_change_old_at(struct btp_tree *t, uint32_t i, apfs_oid_t *old_oid,
    apfs_xid_t *old_xid);

/* the root after the last change, when it is not taken from the caller's own fields */
int btp_root(struct btp_tree *t, uint64_t *addr, apfs_oid_t *oid);

#endif /* _MISCFS_APFS_APFS_TXN_PRIV_H_ */
