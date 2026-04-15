/*
 * monitor.c - Linux Kernel Module for OS-Jackfruit
 * Exposes /dev/container_monitor. Tracks container PIDs,
 * checks RSS periodically, enforces soft/hard memory limits.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/timer.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/sched/mm.h>
#include <linux/pid.h>
#include <linux/mm.h>
#include <linux/jiffies.h>

#include "monitor_ioctl.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("OS-Jackfruit Team");
MODULE_DESCRIPTION("Container memory monitor with soft/hard limits");
MODULE_VERSION("1.0");

#define CHECK_INTERVAL_MS 1000

struct container_entry {
    pid_t  host_pid;
    char   name[64];
    long   soft_limit_kb;
    long   hard_limit_kb;
    int    soft_warned;
    struct list_head list;
};

static LIST_HEAD(container_list);
static DEFINE_MUTEX(list_lock);
static struct timer_list check_timer;

/* ---- RSS helper ---- */
static long get_rss_kb(pid_t pid)
{
    struct task_struct *task;
    struct mm_struct *mm;
    long rss_kb = -1;

    rcu_read_lock();
    task = pid_task(find_vpid(pid), PIDTYPE_PID);
    if (!task) {
        rcu_read_unlock();
        return -1;
    }
    mm = get_task_mm(task);
    rcu_read_unlock();

    if (!mm)
        return -1;

    rss_kb = (get_mm_rss(mm) << PAGE_SHIFT) / 1024;
    mmput(mm);
    return rss_kb;
}

/* ---- Periodic timer callback ---- */
static void check_containers(struct timer_list *t)
{
    struct container_entry *entry, *tmp;
    long rss_kb;

    mutex_lock(&list_lock);
    list_for_each_entry_safe(entry, tmp, &container_list, list) {
        rss_kb = get_rss_kb(entry->host_pid);

        if (rss_kb < 0) {
            pr_info("jackfruit: '%s' (pid=%d) gone, removing\n",
                    entry->name, entry->host_pid);
            list_del(&entry->list);
            kfree(entry);
            continue;
        }

        if (entry->hard_limit_kb > 0 && rss_kb > entry->hard_limit_kb) {
            struct pid *p;
            pr_warn("jackfruit: HARD LIMIT '%s' (pid=%d) rss=%ldKB > %ldKB - SIGKILL\n",
                    entry->name, entry->host_pid, rss_kb, entry->hard_limit_kb);
            p = find_get_pid(entry->host_pid);
            if (p) {
                kill_pid(p, SIGKILL, 1);
                put_pid(p);
            }
            list_del(&entry->list);
            kfree(entry);
            continue;
        }

        if (entry->soft_limit_kb > 0 && rss_kb > entry->soft_limit_kb
                && !entry->soft_warned) {
            pr_warn("jackfruit: SOFT LIMIT '%s' (pid=%d) rss=%ldKB > %ldKB - warning\n",
                    entry->name, entry->host_pid, rss_kb, entry->soft_limit_kb);
            entry->soft_warned = 1;
        }
    }
    mutex_unlock(&list_lock);

    mod_timer(&check_timer, jiffies + msecs_to_jiffies(CHECK_INTERVAL_MS));
}

/* ---- ioctl ---- */
static long monitor_ioctl(struct file *file, unsigned int cmd,
                           unsigned long arg)
{
    if (cmd == MONITOR_REGISTER) {
        struct container_info info;
        struct container_entry *entry;

        if (copy_from_user(&info, (void __user *)arg, sizeof(info)))
            return -EFAULT;

        entry = kzalloc(sizeof(*entry), GFP_KERNEL);
        if (!entry)
            return -ENOMEM;

        entry->host_pid      = info.host_pid;
        entry->soft_limit_kb = info.soft_limit_kb;
        entry->hard_limit_kb = info.hard_limit_kb;
        entry->soft_warned   = 0;
        strncpy(entry->name, info.name, sizeof(entry->name) - 1);
        INIT_LIST_HEAD(&entry->list);

        mutex_lock(&list_lock);
        list_add_tail(&entry->list, &container_list);
        mutex_unlock(&list_lock);

        pr_info("jackfruit: registered '%s' pid=%d soft=%ldKB hard=%ldKB\n",
                entry->name, entry->host_pid,
                entry->soft_limit_kb, entry->hard_limit_kb);
        return 0;
    }

    if (cmd == MONITOR_UNREGISTER) {
        struct container_entry *entry, *tmp;
        pid_t pid;

        if (copy_from_user(&pid, (void __user *)arg, sizeof(pid)))
            return -EFAULT;

        mutex_lock(&list_lock);
        list_for_each_entry_safe(entry, tmp, &container_list, list) {
            if (entry->host_pid == pid) {
                pr_info("jackfruit: unregistered '%s' pid=%d\n",
                        entry->name, pid);
                list_del(&entry->list);
                kfree(entry);
                break;
            }
        }
        mutex_unlock(&list_lock);
        return 0;
    }

    return -EINVAL;
}

/* ---- Misc device ---- */
static const struct file_operations monitor_fops = {
    .owner          = THIS_MODULE,
    .unlocked_ioctl = monitor_ioctl,
};

static struct miscdevice monitor_dev = {
    .minor = MISC_DYNAMIC_MINOR,
    .name  = "container_monitor",
    .fops  = &monitor_fops,
    .mode  = 0666,
};

/* ---- Init / Exit ---- */
static int __init monitor_init(void)
{
    int ret = misc_register(&monitor_dev);
    if (ret) {
        pr_err("jackfruit: failed to register device: %d\n", ret);
        return ret;
    }
    timer_setup(&check_timer, check_containers, 0);
    mod_timer(&check_timer, jiffies + msecs_to_jiffies(CHECK_INTERVAL_MS));
    pr_info("jackfruit: monitor loaded - /dev/container_monitor ready\n");
    return 0;
}

static void __exit monitor_exit(void)
{
    struct container_entry *entry, *tmp;

    del_timer_sync(&check_timer);

    mutex_lock(&list_lock);
    list_for_each_entry_safe(entry, tmp, &container_list, list) {
        list_del(&entry->list);
        kfree(entry);
    }
    mutex_unlock(&list_lock);

    misc_deregister(&monitor_dev);
    pr_info("jackfruit: monitor unloaded\n");
}

module_init(monitor_init);
module_exit(monitor_exit);

