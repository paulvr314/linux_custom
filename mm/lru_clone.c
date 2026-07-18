// SPDX-License-Identifier: GPL-2.0
/*
 * mm/lru_clone.c
 *
 * added by paul
 * linked list that watches for lru list updates and tracks them
 * contains a global lock to prevent concurrency issues
 * nodes contain the index to find the corresponding page in the fenwick tree
 * also handles redrawing the fenwick tree when it hits capacity
 * when this happens (or generally when the lock is held), 
 * new actions are queued and processed after the lock is released
 *
 * design choice: do I 1) make things spin when the flag is not set or
 * 2) always have list ops just go to the queue and have a separate thread that processes the queue?
 */

#include "fenwick.h"

#include <linux/mm.h>
#include <linux/types.h>
#include <linux/list.h>
#include <linux/atomic.h>
#include <linux/spinlock.h>



struct lru_clone_node {
    struct list_head list;
    unsigned long index;
    u64 value;
};

enum lru_operation_type {
    LRU_OP_ADD,
    LRU_OP_ACCESS,
    LRU_OP_PROMOTE,
    LRU_OP_EVICT,
};

struct lru_operation_queue {
    struct list_head *head;
    struct list_head *tail;
    enum lru_operation_type type;
    struct lru_clone_node *node;
};

struct lru_clone {
    struct list_head *head;
    struct list_head *tail;
    struct fenwick_tree *ft;

    atomic_t list_in_use;
    spinlock_t lock;
}