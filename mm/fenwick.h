/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _MM_FENWICK_H
#define _MM_FENWICK_H

#include <linux/types.h>

/*
 * Fenwick tree used by the memory management subsystem to efficiently
 * maintain cumulative page counts for LRU depth calculations.
 *
 * The tree uses 1-based indexing. Index 0 is unused.
 */
struct fenwick_tree {
	unsigned long array_size;
	unsigned long highest_index;
	u64 *array;
};

/*
 * Initialize a Fenwick tree capable of storing up to @size indices.
 *
 * Returns a pointer to the initialized tree on success or NULL on failure.
 */
struct fenwick_tree* fenwick_init(unsigned long size);

/* Free all memory owned by the tree. */
void fenwick_free(struct fenwick_tree *ft);

/*
 * Add a new entry at the logical end of the tree.
 *
 * Returns the assigned index.
 */
unsigned long fenwick_add_new(struct fenwick_tree *ft, u64 value);

/*
 * Move an existing entry to the logical end of the tree.
 *
 * Returns the new index assigned to the entry.
 */
unsigned long fenwick_move_to_back(struct fenwick_tree *ft,
				   unsigned long index,
				   u64 value);

/* Remove an entry from the tree. */
void fenwick_remove(struct fenwick_tree *ft,
		    unsigned long index,
		    u64 value);

/*
 * Return the depth of the entry at @index.
 */
u64 fenwick_get_depth(const struct fenwick_tree *ft,
		      unsigned long index);

#endif /* _MM_FENWICK_H */