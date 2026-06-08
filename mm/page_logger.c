// SPDX-License-Identifier: GPL-2.0
/*
 * mm/page_logger.c
 *
 * added by paul
 * kernel thread that logs page access information to a file every 30 seconds. ()
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

/* Pull in our own declaration (added in Step 4) */
#include <linux/memcontrol.h>

#define PAGE_LOGGER_OUTPUT_PATH  "/var/log/page_logger.txt"
#define PAGE_LOGGER_INTERVAL_MS  30000   /* 30 seconds */

/*
 * page_logger - all state for the logger subsystem.
 *
 * Mirrors the vmpressure pattern: embed state here rather than using
 * scattered globals. spinlock protects tick_count for when we later
 * add per-CPU counter draining from the page access hot path.
 */
struct page_logger {
    spinlock_t          lock;
    unsigned long       tick_count;
    struct task_struct *thread;
};

/* Single global instance. In a later phase this could be
 * embedded in struct mem_cgroup, one per cgroup. */
static struct page_logger page_logger_state;

/*
 * page_logger_write_tick - open the output file and append one line.
 *
 * filp_open / kernel_write is discouraged in production drivers but
 * is the correct approach for a learning/development in-tree tool that
 * needs a persistent log with history (not a pull-on-demand cgroupfs file).
 * See discussion: the pull model via cftype/seq_show cannot accumulate
 * historical entries without a kernel-side ring buffer.
 */
static void page_logger_write_tick(struct page_logger *lg)
{
    struct file *f;
    char buf[256];
    int  len;
    loff_t pos = 0;
    unsigned long tick;

    spin_lock(&lg->lock);
    tick = ++lg->tick_count;
    spin_unlock(&lg->lock);

    len = scnprintf(buf, sizeof(buf),
        "[tick %lu | t=%lld] placeholder: page access data will go here\n",
        tick, (long long)ktime_get_real_seconds());

    /*
     * O_CREAT | O_WRONLY | O_APPEND:
     *   - creates the file if it does not exist
     *   - always appends, never overwrites
     * 0644: rw-r--r-- permissions on creation
     */
    f = filp_open(PAGE_LOGGER_OUTPUT_PATH,
                  O_CREAT | O_WRONLY | O_APPEND, 0644);
    if (IS_ERR(f)) {
        pr_err_ratelimited("page_logger: cannot open %s: %ld\n",
                           PAGE_LOGGER_OUTPUT_PATH, PTR_ERR(f));
        return;
    }

    kernel_write(f, buf, len, &pos);
    filp_close(f, NULL);
}

/*
 * page_logger_thread_fn - the kthread body.
 *
 * Runs until kthread_stop() is called from page_logger_cleanup().
 * msleep_interruptible() yields the CPU properly; it wakes early
 * if a signal (including the kthread stop signal) arrives, which
 * is why we re-check kthread_should_stop() at the top of the loop
 * rather than relying solely on the sleep duration.
 */
static int page_logger_thread_fn(void *data)
{
    struct page_logger *lg = (struct page_logger *)data;

    pr_info("page_logger: thread started\n");

    while (!kthread_should_stop()) {
        page_logger_write_tick(lg);
        msleep_interruptible(PAGE_LOGGER_INTERVAL_MS);
    }

    pr_info("page_logger: thread stopped\n");
    return 0;
}

/*
 * page_logger_init - initialise state and start the kthread.
 *
 * Called from mem_cgroup_init() in mm/memcontrol.c (Step 3).
 * Using subsys_initcall ordering means memcg infrastructure is
 * already set up by the time we run, and we run before userspace starts.
 */
void page_logger_init(void)
{
    struct page_logger *lg = &page_logger_state;

    spin_lock_init(&lg->lock);
    lg->tick_count = 0;
    lg->thread     = NULL;

    lg->thread = kthread_run(page_logger_thread_fn, lg, "page_logger");
    if (IS_ERR(lg->thread)) {
        pr_err("page_logger: failed to start kthread: %ld\n",
               PTR_ERR(lg->thread));
        lg->thread = NULL;
        return;
    }

    pr_info("page_logger: initialised, logging to %s every %d ms\n",
            PAGE_LOGGER_OUTPUT_PATH, PAGE_LOGGER_INTERVAL_MS);
}

/*
 * page_logger_cleanup - stop the kthread cleanly.
 *
 * kthread_stop() sets the stop flag and blocks until the thread returns.
 * This mirrors vmpressure_cleanup()'s use of flush_work().
 */
void page_logger_cleanup(void)
{
    struct page_logger *lg = &page_logger_state;

    if (lg->thread) {
        kthread_stop(lg->thread);
        lg->thread = NULL;
    }
}