#ifndef MONITOR_IOCTL_H
#define MONITOR_IOCTL_H

#include <linux/ioctl.h>

/* ioctl magic number — unique to avoid collisions */
#define MONITOR_MAGIC 0xCF

/*
 * Payload sent from user space (engine.c) to kernel space (monitor.c)
 * for each container registration.
 */
struct container_info {
    pid_t  host_pid;           /* host PID of the container init process */
    char   name[64];           /* human-readable container name           */
    long   soft_limit_kb;      /* RSS soft limit in kilobytes             */
    long   hard_limit_kb;      /* RSS hard limit in kilobytes             */
};

/*
 * ioctl commands
 *
 *  MONITOR_REGISTER   — register a new container PID with limits
 *  MONITOR_UNREGISTER — remove a container from the tracked list
 */
#define MONITOR_REGISTER   _IOW(MONITOR_MAGIC, 1, struct container_info)
#define MONITOR_UNREGISTER _IOW(MONITOR_MAGIC, 2, pid_t)

/* Device path the kernel module exposes */
#define MONITOR_DEVICE "/dev/container_monitor"

#endif /* MONITOR_IOCTL_H */
