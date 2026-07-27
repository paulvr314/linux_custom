// SPDX-License-Identifier: GPL-2.0
/*
 * mm/lru_clone.c
 *
 * added by paul
 * THIS CODE IS INCOMPLETE
 * it has not been tested and probably has a few bugs
 * it is more of a proof of concept, to show how I would implement
 * fast page depth tracking in linux
 * 
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
#include <linux/string.h>

#define LRU_OP_QUEUE_SIZE = 16384
#define LRU_OP_QUEUE_PCT_CAPACITY = .2

struct lru_clone_node {
    struct list_head lru;
    unsigned long index;
    u64 value;
};

struct lru_clone {
    struct list_head lru;
    struct circ_buf op_queue;
    struct fenwick_tree *ft;

    struct task_struct *upkeep_thread;
    wait_queue_head_t   upkeep_wait;

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

//fenwick size probably wants to depend on cache size so is a variable
struct lru_clone* lru_clone_init(unsigned long fenwick_size) {
    //allocate lru_clone struct
    struct lru_clone *lc = kmalloc(sizeof(struct lru_clone), GFP_KERNEL);
    if (!lc) {
        return NULL;
    }

    //allocate operation queue
    lc->op_queue.buf = kmalloc(LRU_OP_QUEUE_SIZE * sizeof(struct lru_op), GFP_KERNEL);
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

//---------fenwick tree redraw functions

//assumes we have set the flag here
static inline bool fenwick_needs_redraw(struct lru_clone *lc) {
    return lc->ft->index = lc->ft->array_size - LRU_OP_QUEUE_SIZE;
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
    struct circ_buf *cb = &lc->op_queue;
    unsigned long head, tail;

    spin_lock(&lc->lock);

    head = cb->head;
    tail = READ_ONCE(cb->tail);

    //assume that overflows do not happen in practice
    struct lru_op *slot = &((struct lru_op *)cb->buf)[head];
    slot->op_fn = op_fn;
    slot->node = node;
    slot->timestamp = timestamp;

    smp_store_release(&cb->head, (head + 1) & (LRU_OP_QUEUE_SIZE - 1));
    spin_unlock(&lc->lock);

    //check queue needs to be emptied
    if (CIRC_SPACE(head, tail, LRU_OP_QUEUE_SIZE) <= LRU_OP_QUEUE_SIZE * LRU_OP_QUEUE_PCT_CAPACITY) {
        //only call redraw if we were the thread able to set the flag
        if (atomic_cmpxchg(lc->list_in_use, 0, 1) == 0) {
            wake_up(&lc->upkeep_wait);
        }
    }
}

//redraws fenwick tree in place
static void redraw_fenwick_tree(struct lru_clone *lc) {
    struct lru_clone_node *node;
    unsigned long i, parent, n = 0;
    u64 *array = lc->ft->array;
    unsigned long size = lc->ft->array_size;

    //zero the array first
    memset(array, 0, (size + 1) * sizeof(array[0]));

    //o(n) algorithm for fast draw of fenwick tree
    list_for_each_entry(node, &lc->lru, lru) {
        n++;
        array[n] = node->value;
        node->index = n;
    }

    for (i = 1; i <= size; i++) {
        parent = i + (i & (-i));
        if (parent <= size)
            array[parent] += array[i];
    }

    //set ft internal index
    lc->ft->index = n;
}

//check empty opqueue -> check should redraw ft -> redraw ft -> set list in use back to 0
static void do_lru_clone_upkeep(struct lru_clone *lc) {
    op_queue_clear(lc);
    if (fenwick_needs_redraw(lc))
        redraw_fenwick_tree(lc);
    atomic_set(&lc->list_in_use, 0);
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


//------------------------------------thread stuff ---------------------------------------


//main loop
static int lru_clone_upkeep_thread(void *data)
{
    struct lru_clone *lc = data;

    while (!kthread_should_stop()) {
        wait_event_interruptible(lc->upkeep_wait,
            atomic_read(&lc->list_in_use) || kthread_should_stop());

        if (kthread_should_stop())
            break;

        atomic_set(&lc->list_in_use, 0);
        do_lru_clone_upkeep(lc);
    }

    return 0;
}

//thread init function
int lru_clone_upkeep_init(struct lru_clone *lc)
{
    init_waitqueue_head(&lc->upkeep_wait);
    atomic_set(&lc->upkeep_pending, 0);

    lc->upkeep_thread = kthread_run(lru_clone_upkeep_thread, lc,
                                     "lru_clone_upkeep");
    if (IS_ERR(lc->upkeep_thread)) {
        int err = PTR_ERR(lc->upkeep_thread);
        lc->upkeep_thread = NULL;
        return err;
    }

    return 0;
}

//teardown
void lru_clone_upkeep_stop(struct lru_clone *lc)
{
    if (lc->upkeep_thread) {
        kthread_stop(lc->upkeep_thread);
        lc->upkeep_thread = NULL;
    }
}

