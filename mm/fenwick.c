// SPDX-License-Identifier: GPL-2.0
/*
 * mm/fenwick.c
 *
 * added by paul
 * fenwick tree with a builtin indexing system used to report page depth in logarithmic time
 * a method is implemented in (somewhere -- tbd) to walk the lru list and populate the tree in o(n) time
 */

#include "fenwick.h"

#include <linux/mm.h>
#include <linux/types.h>


int fenwick_init(struct fenwick_tree *ft, unsigned long size)
{
    ft->array_size = size;
    ft->highest_index = 0;
    ft->array = kvcalloc(size + 1, sizeof(u64), GFP_KERNEL);
    if (!ft->array)
        return -ENOMEM;
    return 0;
}

//static functions assume index <= highest index

static void fenwick_add(struct fenwick_tree *ft, unsigned long index, u64 value)
{
    while (index <= ft->array_size) {
        ft->array[index] += value;
        index += index & -index;
    }
}

static void fenwick_sub(struct fenwick_tree *ft, unsigned long index, u64 value)
{
    while (index <= ft->array_size) {
        ft->array[index] -= value;
        index += index & -index;
    }
}

static u64 fenwick_sum(struct fenwick_tree *ft, unsigned long index)
{
    u64 sum = 0;
    while (index > 0) {
        sum += ft->array[index];
        index -= index & -index;
    }
    return sum;
}

void fenwick_free(struct fenwick_tree *ft)
{
    kvfree(ft->array);
    ft->array = NULL;
    ft->array_size = 0;
    ft->highest_index = 0;
}

//-------------------specialized functions for page depth tracking-------------------


//adds a new element at highest_index + 1, returns the index of the new element
unsigned long fenwick_add_new(struct fenwick_tree *ft, u64 value)
{
    ft->highest_index++;
    fenwick_add(ft, ft->highest_index, value);
    return ft->highest_index;
}

//moves page to the back of the array, returns the new index of the page
unsigned long fenwick_move_to_back(struct fenwick_tree *ft, unsigned long index, u64 value)
{
    fenwick_sub(ft, index, value);
    return fenwick_add_new(ft, value);
}

void fenwick_remove(struct fenwick_tree *ft, unsigned long index, u64 value)
{
    fenwick_sub(ft, index, value);
}

u64 fenwick_get_depth(struct fenwick_tree *ft, unsigned long index)
{
    return fenwick_sum(ft, ft->highest_index) - fenwick_sum(ft, index);
}

