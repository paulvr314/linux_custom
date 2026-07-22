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

#define FENWICK_REDRAW_PCT_CAPACITY 0.9

struct lru_clone_node {
    struct list_head lru;
    unsigned long index;
    u64 value;
};

struct lru_clone {
    struct list_head lru;
    struct circ_buf op_queue;
    struct fenwick_tree *ft;

    //usage: the list_in_use flag is set when we traverse the list to redraw the fenwick tree,
    //the spinlock must be held to touch anything in lru_clone
    atomic_t list_in_use;
    spinlock_t lock;
};

struct lru_op {
    void (*op_fn)(struct lru_clone *lc, struct lru_clone_node *node);
    struct lru_clone_node *node;
};

//fenwick size and op queue size are parameters because they probably want to depend on cache size
struct lru_clone* lru_clone_init(unsigned long fenwick_size, unsigned long op_queue_size) {
    //allocate lru_clone struct
    struct lru_clone *lc = kmalloc(sizeof(struct lru_clone), GFP_KERNEL);
    if (!lc) {
        return NULL;
    }

    //allocate operation queue
    lc->op_queue.buf = kmalloc(op_queue_size * sizeof(struct lru_op), GFP_KERNEL);
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

//do methods assume that either we hold the lock or the list_in_use flag

//performs an add operation to the lru list and fenwick tree
static void do_lru_add(struct lru_clone *lc, struct lru_clone_node *node) {
    list_add_tail(&node->lru, &lc->lru);
    node->index = fenwick_add_new(lc->ft, node->value);
}

//performs an access operation to lru list and fenwick tree
static void do_lru_access(struct lru_clone *lc, struct lru_clone_node *node) {
    list_move_tail(&node->lru, &lc->lru);
    node->index = fenwick_move_to_back(&lc->ft, node->index, node->value);
}

//performs an evict operation to lru list and fenwick tree
static void do_lru_evict(struct lru_clone *lc, struct lru_clone_node *node) {
    list_del(&node->lru);
    fenwick_remove(&lc->ft, node->index, node->value);
}

//---------fenwick tree redraw functions todo -- clarify what in here needs a lock

//assumes lock is held
static inline bool fenwick_needs_redraw(struct lru_clone *lc) {
    return lc->ft->highest_index >= FENWICK_REDRAW_PCT_CAPACITY * lc->ft->array_size;
}

//I chose to allow concurrent writes to the op queue while clearling, but TODO: check that this will be okay
static void clear_op_queue(struct lru_clone *lc)
{
    struct circ_buf *cb = &lc->op_queue;
    unsigned long head, tail;

    while (1) {
        head = smp_load_acquire(&cb->head);   /* pairs with producer's release */
        tail = cb->tail;

        if (!CIRC_CNT(head, tail, LRU_OP_QUEUE_SIZE))
            break;

        struct lru_op op = ((struct lru_op *)cb->buf)[tail];

        smp_store_release(&cb->tail,
                           (tail + 1) & (LRU_OP_QUEUE_SIZE - 1));

        op.op_fn(lc, op.node);
    }
}





//----------flag and lock logistics



//----------callable functions for outside use


