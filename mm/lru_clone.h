// SPDX-License-Identifier: GPL-2.0
/*
 * include/linux/lru_clone.h
 * 
 * Public API for fast page depth tracking on an LRU list using a Fenwick tree.
 */

#ifndef _LINUX_LRU_CLONE_H
#define _LINUX_LRU_CLONE_H

#include <linux/types.h>

/* 
 * Opaque forward declarations.
 * The actual struct definitions are hidden in mm/lru_clone.c to enforce 
 * encapsulation and prevent external code from bypassing the operation queue.
 */
struct lru_clone;
struct lru_clone_node;

/* Initialization and Teardown */
struct lru_clone* lru_clone_init(unsigned long fenwick_size);
void lru_clone_destroy(struct lru_clone *lc);

/* Thread Management */
int lru_clone_upkeep_init(struct lru_clone *lc);
void lru_clone_upkeep_stop(struct lru_clone *lc);

/* 
 * Core API 
 * These functions enqueue operations to the ring buffer locklessly (for the caller)
 * and return immediately. 
 */

/**
 * lru_clone_add_new - Track a newly allocated or promoted page
 * @lc: Pointer to the lru_clone context
 * @value: The initial weight/value of the page
 * @timestamp: Time of access
 *
 * Returns a pointer to the tracking node, which the caller must store
 * (e.g., inside the page's cgroup tracking struct) for future accesses.
 */
struct lru_clone_node* lru_clone_add_new(struct lru_clone *lc, u64 value, 
                                         unsigned long timestamp);

/**
 * lru_clone_access - Record a hit on an existing tracked page
 * @lc: Pointer to the lru_clone context
 * @node: The node previously returned by lru_clone_add_new
 * @timestamp: Time of access
 */
void lru_clone_access(struct lru_clone *lc, struct lru_clone_node *node, 
                      unsigned long timestamp);

/**
 * lru_clone_evict - Stop tracking a page and free its node
 * @lc: Pointer to the lru_clone context
 * @node: The node to evict
 * @timestamp: Time of eviction
 */
void lru_clone_evict(struct lru_clone *lc, struct lru_clone_node *node, 
                     unsigned long timestamp);

#endif /* _LINUX_LRU_CLONE_H */