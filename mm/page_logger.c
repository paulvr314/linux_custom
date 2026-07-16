// SPDX-License-Identifier: GPL-2.0
/*
 * mm/page_logger.c
 *
 * added by paul
 * kernel thread that logs page access information to a file every 60 seconds. ()
 */

#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/spinlock.h>
#include <linux/fs.h>
#include <linux/timekeeping.h>
#include <linux/printk.h>
#include <linux/uaccess.h>
#include "internal.h"          /* mm-internal helpers, already in mm/ */
#include <linux/pagewalk.h>     /* mm_walk_ops, walk_page_range      */
#include <linux/hugetlb.h>  

#include <linux/memcontrol.h>   /* mem_cgroup, mem_cgroup_from_css        */
#include <linux/cgroup.h>       /* css_for_each_descendant_pre,
                                   css_task_iter_start/next/end            */
#include <linux/sched/mm.h>     /* get_task_mm(), mmput()                  */
#include <linux/mm.h>           /* mm_struct, mmap_read_lock/unlock        */

#define PAGE_LOGGER_OUTPUT_PATH  "/var/log/page_logger.txt"
#define PAGE_LOGGER_INTERVAL_MS  60000   /* 60 seconds */

//struct to store data during page walk, to be used in walk_page_range
struct page_logger_walk_data {
    struct mm_struct *mm;
    unsigned long     count;   /* incremented for each accessed file page */
};

//callback for pte
static int page_logger_pte_entry(pte_t *pte, unsigned long addr,
                                 unsigned long next, struct mm_walk *walk)
{
    struct page_logger_walk_data *wd = walk->private;

    if (!pte_present(ptep_get(pte)))
        return 0;

    if (ptep_clear_flush_young(walk->vma, addr, pte))
        wd->count++;

    return 0;
}

//single thread for page logging
static struct task_struct *page_logger_thread;

//walk_ops struct for walk_page_range, to be used in process_mm()
static const struct mm_walk_ops page_logger_walk_ops = {
    .pgd_entry = NULL,
    .p4d_entry = NULL,
    .pud_entry = NULL,
    .pmd_entry = NULL,
    .pte_entry = page_logger_pte_entry,
    .pte_hole = NULL,
    .hugetlb_entry = NULL,
    .test_walk = NULL,
    .pre_vma = NULL,
    .post_vma = NULL,
    .walk_lock = PGWALK_WRLOCK
};


//walk the page table
static u64 process_mm(struct mm_struct *mm)
{
    struct page_logger_walk_data wd = {
        .mm    = mm,
        .count = 0,
    };

    mmap_write_lock(mm);
    walk_page_range(mm, 0, TASK_SIZE, &page_logger_walk_ops, &wd);
    mmap_write_unlock(mm);

    pr_info("process_mm: walked page table for mm %p, total count: %llu\n", mm, (unsigned long long)wd.count);

    return wd.count;
}


//sum counts for each process in cgroup
static void scan_memcg(struct mem_cgroup *memcg)
{
    struct css_task_iter it;
    struct task_struct  *task;
    struct mm_struct    *mm;
    u64                  total = 0;

    if (!memcg->page_logger_enabled)
        return;

    css_task_iter_start(&memcg->css, CSS_TASK_ITER_PROCS, &it);

    while ((task = css_task_iter_next(&it))) {
        mm = get_task_mm(task);
        if (!mm)
            continue;

        total += process_mm(mm);
        mmput(mm);
    }

    css_task_iter_end(&it);

    atomic64_set(&memcg->nr_unique_pages, (s64)total);
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
    page_logger_thread = kthread_run(page_logger_thread_fn,
                                           NULL, "page_logger");
    if (IS_ERR(page_logger_thread)) {
        pr_err("page_logger: failed to start kthread: %ld\n",
               PTR_ERR(page_logger_thread));
        page_logger_thread = NULL;
    }
}

void page_logger_cleanup(void)
{
    if (page_logger_thread) {
        kthread_stop(page_logger_thread);
        page_logger_thread = NULL;
    }
}