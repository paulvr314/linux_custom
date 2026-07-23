// SPDX-License-Identifier: GPL-2.0
/*
 * mm/lru_clone.c
 *
 * added by paul
 * design: on page access, an operation is added to the operation queue
 * every once in a while, or when the op queue gets too full, the opqueue is emptied
 * all operations are performed on the lru list (linked list) and the fenwick tree
 * then if the remaining space in the fenwick tree is smaller than the opqueue size,
 * the fenwick tree is redrawn in o(n) time by traversing the lru list 
 * 
 * for this to work opqueue needs to be big enough that it doesn't need to be emptied while ft is being redrawn
 *
 * handling concurrency:
 * enqueue opqueue: must grab spinlock and use smp function to sync with reader
 * dequeue opqueue: must use smp function to sync with writer, can be done without lock
 * touching lru list/fenwick tree: to prevent race conditions, must first set list_in_use flag
 * using atomic_cmpxchg -- DO NOT use atomic_read and atomic_set as two different operations
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

struct lru_clone {
    struct list_head lru;
    struct circ_buf op_queue;
    struct fenwick_tree *ft;

    //usage: the list_in_use flag is set when we traverse the list to redraw the fenwick tree,
    //the spinlock must be held to touch anything in lru_clone
    atomic_t list_in_use;
    spinlock_t lock;
};

//you could make this smaller by using an enumeration rather than a function pointer but I did not do this
struct lru_op {
    void (*op_fn)(struct lru_clone *lc, struct lru_clone_node *node);
    struct lru_clone_node *node;
    unsigned long timestamp;
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
    kfree(node);
}

//---------fenwick tree redraw functions todo -- clarify what in here needs a lock

//assumes lock is held
static inline bool fenwick_needs_redraw(struct lru_clone *lc) {
    return lc->ft->highest_index >= FENWICK_REDRAW_PCT_CAPACITY * lc->ft->array_size;
}

//clears queue, assumes list_in_use is set, but does not need to hold lock
static void op_queue_clear(struct lru_clone *lc)
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

//does concurrency safe enqueue then notifies lru draining thread if list is getting full
static void op_queue_enqueue(
    struct lru_clone *lc, 
    void (*op_fn)(struct lru_clone *lc, struct lru_clone_node *node),
    struct lru_clone_node *node,
    unsigned long timestamp
) {
    //do circ buf setup
    //grab lock
    //enqueue
    //release lock
    //check queue needs to be emptied
    //if so, try to set list in use flag
    //if we can set the list in use flag, then wake up queue clearing thred
}

static void redraw_fenwick_tree(struct lru_clone *lc) {
    //this will be called 
    atomic_set(&lc->list_in_use, 1); 
    
}

//check should empty opqueue -> empty opqueue -> check should redraw ft -> redraw ft
static void do_lru_clone_upkeep(struct lru_clone *lc) {

}


//----------handling for page promotion and eviction events

static struct lru_clone_node* init_lru_clone_node(u64 value) {
    struct lru_clone_node *node = kmalloc(sizeof(struct lru_clone_node), GFP_KERNEL);
    node->value = value;
    return node;
}

//handles a new page being accessed, and returns a pointer to its lru_clone_node
struct lru_clone_node* lru_clone_add_new(struct lru_clone *lc, u64 value, unsigned long timestamp) {
    struct lru_clone_node *node = init_lru_clone_node(value);
    op_queue_enqueue(lc, do_lru_add, node, timestamp);
    return node;
}

//handles an existing page being accessed, returns a pointer to its lru_clone_node
void lru_clone_access(struct lru_clone *lc, struct lru_clone_node *node, unsigned long timestamp) {
    op_queue_enqueue(lc, do_lru_access, node, timestamp);
}

//handles eviction and frees the associated node.
void lru_clone_evict(struct lru_clone *lc, struct lru_clone_node *node, unsigned long timestamp) {
    op_queue_enqueue(lc, do_lru_evict, node, timestamp);
}