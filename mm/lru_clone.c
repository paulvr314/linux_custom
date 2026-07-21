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
#include <linux/circ_buf.h>



struct lru_clone_node {
    struct list_head lru;
    unsigned long index;
    u64 value;
};

enum lru_operation_type {
    LRU_OP_ADD,
    LRU_OP_ACCESS,
    LRU_OP_EVICT,
};

struct lru_operation {
    enum lru_operation_type type;
    struct lru_clone_node *node;
};

struct lru_clone {
    struct list_head lru;
    struct circ_buf op_queue;
    struct fenwick_tree *ft;

    //usage: the list_in_use flag is set when we traverse the list to redraw the fenwick tree,
    //the spinlock must be held to touch anything in lru_clone
    atomic_t list_in_use;
    spinlock_t lock;
}

struct lru_clone* lru_clone_init(unsigned long fenwick_size, unsigned long op_queue_size) {
    //allocate lru_clone struct
    struct lru_clone *lc = kmalloc(sizeof(struct lru_clone), GFP_KERNEL);
    if (!lc) {
        return NULL;
    }

    //allocate operation queue
    lc->op_queue.buf = kmalloc(op_queue_size * sizeof(struct lru_operation), GFP_KERNEL);
    if (!lc->op_queue.buf) {
        kfree(lc);
        return NULL;
    }
    lc->op_queue.head = 0;
    lc->op_queue.tail = 0;

    //allocate fenwick tree
    lc->ft = fenwick_init(fenwick_size);
    if (!lc->ft) {
        kfree(lc);
        kfree(lc->op_queue.buf);
        return NULL;
    }

    //initialize other members
    INIT_LIST_HEAD(&lc->lru);
    atomic_set(&lc->list_in_use, 0);
    spin_lock_init(&lc->lock);
    return lc;
}

//do methods assume we have the go ahead to perform them, and update the underlying fenwick tree

//adds a new node to the end of the list, returns the new node
static struct lru_clone_node* do_lru_add(struct lru_clone *lc, u64 value) {
    struct lru_clone_node *node = kmalloc(sizeof(struct lru_clone_node), GFP_KERNEL);
    if (!node) {
        return NULL;
    }
    node->value = value;
    node->index = fenwick_add_new(lc->ft, value);
    list_add_tail(&node->lru, &lc->lru);
    return node;
}