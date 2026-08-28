// SPDX-License-Identifier: GPL-2.0
/*
 * mm/mpc_endpoint.c
 *
 * handles the collection and reporting of the mpc curve for a cgroup.
 * puts page depths into bins (probably linearly spaced) and exports results to cgroup fs.
 */

#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/timekeeping.h>
#include <linux/printk.h>
#include <linux/uaccess.h>

#include <linux/types.h>
#include <linux/atomic.h>
#include <linux/memcontrol.h>
#include <linux/mm_inline.h>
#include <linux/mmzone.h>
#include <linux/swap.h>

#include "mpc.h"


/* ---------------------------------------------------------------------
 * definitions and Data structures
 * --------------------------------------------------------------------- */

#define THIRTY_SECOND_INTERVAL_MS 30000 

struct mpc_endpoint {
	atomic_t depth_bins[DEPTH_NR_BINS];
	u32 binwidth;
	u32 max_depth_bin;
	bool enabled;
    struct task_struct *thread;
};

/* ---------------------------------------------------------------------
 * MGLRU enabled checks
 * --------------------------------------------------------------------- */

#ifdef CONFIG_LRU_GEN

static bool mpc_mglru_active(void)
{
    return lru_gen_enabled();
}

#else

static inline bool mpc_mglru_active(void)
{
    return false;
}

#endif

/* ---------------------------------------------------------------------
 * buffer management
 * --------------------------------------------------------------------- */

//get total number of pages -- this may be too slow to run on every access
//in the future I may want to precalcuate this to save time
static unsigned long mpc_sum_anon_gens(struct lru_gen_folio *lrugen,
                                        unsigned long from_seq,
                                        unsigned long to_seq)
{
    unsigned long pages = 0;
    unsigned long seq;
    int zone;

    for (seq = from_seq; seq <= to_seq; seq++) {
        int g = lru_gen_from_seq(seq);
        for (zone = 0; zone < MAX_NR_ZONES; zone++)
            pages += lrugen->nr_pages[g][0][zone]; 
    }
    return pages;
}

//checks that page is anon and that mpc is enabled for the memcg
static inline bool mpc_should_track(struct folio *folio, struct mem_cgroup *memcg)
{
    if (folio_is_file_lru(folio))
        return false;
    if (!memcg || !memcg->mpc || !memcg->mpc->enabled)
        return false;
    return true;
}

void record_depth(struct mpc_endpoint *mpc, u32 depth)
{
	u32 bin;

	if (!mpc->enabled)
		return;

	bin = depth / mpc->binwidth;
	if (bin > mpc->max_depth_bin)
		bin = mpc->max_depth_bin;

	atomic_inc(&mpc->depth_bins[bin]);
}

void mpc_hook_first_access(struct folio *folio)
{
    struct mem_cgroup *memcg = folio_memcg(folio);
    if (mpc_should_track(folio, memcg))
        record_depth(memcg->mpc, 0);
}

void mpc_hook_from_gen(struct folio *folio, unsigned long old_gen, 
    unsigned long new_gen, struct lru_gen_folio *lrugen, struct mem_cgroup *memcg)
{
    if (!mpc_should_track(folio, memcg))
        return;

    //younger gens + half of own gen
    unsigned long depth = mpc_sum_anon_gens(lrugen, old_gen + 1, new_gen);
    depth += mpc_sum_anon_gens(lrugen, old_gen, old_gen) / 2;
    record_depth(memcg->mpc, depth);
}

void mpc_hook_ws_refault(struct folio *folio, struct lru_gen_folio *lrugen)
{
    struct mem_cgroup *memcg = folio_memcg(folio);
    if (!mpc_should_track(folio, memcg))
        return;

    //total pages in mem
    unsigned long depth = mpc_sum_anon_gens(lrugen, lrugen->min_seq[0], lrugen->max_seq);
    record_depth(memcg->mpc, depth);
}


void mpc_hook_slow_refault(struct folio *folio, struct lru_gen_folio *lrugen)
{
    struct mem_cgroup *memcg = folio_memcg(folio);
    if (!mpc_should_track(folio, memcg))
        return;

    //total pages in mem + half of total swap pages
    unsigned long depth = mpc_sum_anon_gens(lrugen, lrugen->min_seq[0], lrugen->max_seq);
    depth += (total_swap_pages - get_nr_swap_pages()) / 2;
    record_depth(memcg->mpc, depth);
}

/* ---------------------------------------------------------------------
 * Thread stuff (handles file export every 30s)
 * --------------------------------------------------------------------- */

  
static int mpc_thread_fn(void *data) 
{
    pr_info("page_logger: thread started\n");

    int i = 0;

    //note this is doing nothing at the moment, later will do aditional table walks if needed.

    while (true) {
        msleep_interruptible(THIRTY_SECOND_INTERVAL_MS);
        i++;
    }

    pr_info("page_logger: thread stopping\n");
    return 0;
}


/* ---------------------------------------------------------------------
 * Init / teardown 
 * --------------------------------------------------------------------- */

 struct mpc_endpoint *mpc_endpoint_alloc(u32 binwidth, u32 max_depth, struct mem_cgroup *memcg)
 {
	if (!mpc_mglru_active())
		return NULL;

	struct mpc_endpoint *mpc = kzalloc(sizeof(struct mpc_endpoint), GFP_KERNEL);
	if (!mpc)
		return NULL;

	mpc->binwidth = binwidth;
	mpc->max_depth_bin = max_depth / binwidth;
	mpc->enabled = true;

    mpc->thread = kthread_run(mpc_thread_fn, NULL, "mpc_thread");

	return mpc;
 }

 void mpc_endpoint_free(struct mpc_endpoint *mpc)
 {
    kthread_stop(mpc->thread);
	kfree(mpc);
 }