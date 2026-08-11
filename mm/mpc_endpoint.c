// SPDX-License-Identifier: GPL-2.0
/*
 * mm/mpc_endpoint.c
 *
 * handles the collection and reporting of the mpc curve for a cgroup.
 * puts page depths into bins (probably linearly spaced) and exports results to cgroup fs.
 */


#include <linux/types.h>
#include <linux/atomic.h>


/* ---------------------------------------------------------------------
 * Tunables
 * --------------------------------------------------------------------- */


#define DEPTH_NR_BINS         1000          //1000 u32s make mpc_endpoint about 1 page


/* ---------------------------------------------------------------------
 * Data structures
 * --------------------------------------------------------------------- */

struct mpc_endpoint {
	atomic_t depth_bins[DEPTH_NR_BINS];
	u32 binwidth;
	u32 max_depth_bin;
	bool enabled;
};

/* ---------------------------------------------------------------------
 * buffer management
 * --------------------------------------------------------------------- */

void record_depth(struct mpc_endpoint *ep, u32 depth)
{
	u32 bin;

	if (!ep->enabled)
		return;

	bin = depth / ep->binwidth;
	if (bin > ep->max_depth_bin)
		bin = ep->max_depth_bin;

	atomic_inc(&ep->depth_bins[bin]);
}

/* ---------------------------------------------------------------------
 * Export to cgroup fs
 * --------------------------------------------------------------------- */




/* ---------------------------------------------------------------------
 * Init / teardown 
 * --------------------------------------------------------------------- */

 struct mpc_endpoint *mpc_endpoint_alloc(u32 binwidth, u32 max_depth)
 {
	struct mpc_endpoint *ep = kzalloc(sizeof(struct mpc_endpoint), GFP_KERNEL);
	if (!ep)
		return NULL;

	ep->binwidth = binwidth;
	ep->max_depth_bin = max_depth / binwidth;
	ep->enabled = true;

	return ep;
 }