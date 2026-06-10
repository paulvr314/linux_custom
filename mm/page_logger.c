// SPDX-License-Identifier: GPL-2.0
/*
 * mm/page_logger.c
 *
 * added by paul
 * kernel thread that logs page access information to a file every 60 seconds. ()
 *
 * TODO: replace placeholder output with system information
 */

#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/spinlock.h>
#include <linux/fs.h>
#include <linux/timekeeping.h>
#include <linux/printk.h>
#include <linux/uaccess.h>
#include "internal.h"          /* mm-internal helpers, already in mm/ */

#include <linux/memcontrol.h>   /* mem_cgroup, mem_cgroup_from_css        */
#include <linux/cgroup.h>       /* css_for_each_descendant_pre,
                                   css_task_iter_start/next/end            */
#include <linux/sched/mm.h>     /* get_task_mm(), mmput()                  */
#include <linux/mm.h>           /* mm_struct, mmap_read_lock/unlock        */
#include "internal.h"

#define PAGE_LOGGER_OUTPUT_PATH  "/var/log/page_logger.txt"
#define PAGE_LOGGER_INTERVAL_MS  60000   /* 60 seconds */

/*
 * page_logger - all state for the logger subsystem.
 */
struct page_logger {
    struct task_struct *thread;
};

static struct page_logger page_logger_state;

/* -----------------------------------------------------------------------
 * process_mm - placeholder for real page table walking.
 * Returns a placeholder count; will later return unique file count
 * derived from page table inspection.
 * ----------------------------------------------------------------------- */
static u64 process_mm(struct mm_struct *mm)
{
    /*
     * Future: mmap_read_lock(mm), walk VMAs + PTEs, count unique
     * file-backed pages with pte_young(), mmap_read_unlock(mm).
     * For now, return a placeholder.
     */
    return 1;
}


/* -----------------------------------------------------------------------
 * scan_memcg - walk all tasks in one memcg, accumulate a count,
 * then store it atomically into the per-cgroup state.
 * ----------------------------------------------------------------------- */
static void scan_memcg(struct mem_cgroup *memcg)
{
    struct css_task_iter it;
    struct task_struct  *task;
    struct mm_struct    *mm;
    u64                  total = 0;

    css_task_iter_start(&memcg->css, CSS_TASK_ITER_PROCS, &it);

    while ((task = css_task_iter_next(&it))) {
        mm = get_task_mm(task);
        if (!mm)
            continue;

        total += process_mm(mm);
        mmput(mm);
    }

    css_task_iter_end(&it);

    /*
     * Store the result into this cgroup's embedded state.
     * atomic64_set is used rather than atomic64_add because each tick
     * produces a fresh count, not an increment of a running total.
     * seq_show on the read side uses atomic64_read, so no lock needed.
     */
    atomic64_set(&memcg->page_logger.unique_files, (s64)total);
}


//calls scan_memcg for every cgroup
static void scan_all_memcgs(void)
{
    struct mem_cgroup           *memcg;
    struct cgroup_subsys_state  *css;

    rcu_read_lock();

    css_for_each_descendant_pre(css, &root_mem_cgroup->css) {
        if (!css_tryget_online(css))
            continue;

        memcg = mem_cgroup_from_css(css);

        rcu_read_unlock();
        scan_memcg(memcg);
        css_put(css);
        rcu_read_lock();
    }

    rcu_read_unlock();
}


/*calls scan_all_memcgs every 60 seconds */
static int page_logger_thread_fn(void *data)
{
    pr_info("page_logger: thread started\n");

    while (!kthread_should_stop()) {
        scan_all_memcgs();
        msleep_interruptible(PAGE_LOGGER_INTERVAL_MS);
    }

    pr_info("page_logger: thread stopping\n");
    return 0;
}


//init and cleanup functions for the module, to start and stop the kthread
void page_logger_init(void)
{
    page_logger_state.thread = kthread_run(page_logger_thread_fn,
                                           NULL, "page_logger");
    if (IS_ERR(page_logger_state.thread)) {
        pr_err("page_logger: failed to start kthread: %ld\n",
               PTR_ERR(page_logger_state.thread));
        page_logger_state.thread = NULL;
    }
}

void page_logger_cleanup(void)
{
    if (page_logger_state.thread) {
        kthread_stop(page_logger_state.thread);
        page_logger_state.thread = NULL;
    }
}