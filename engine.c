/*
 * engine.c - User-space supervisor for OS-Jackfruit
 *
 * Usage:
 *   sudo ./engine supervisor <rootfs>
 *   sudo ./engine start <name> <rootfs> <cmd> [--soft KB] [--hard KB]
 *   sudo ./engine run   <name> <rootfs> <cmd> [--soft KB] [--hard KB]
 *   sudo ./engine ps
 *   sudo ./engine logs  <name>
 *   sudo ./engine stop  <name>
 *
 * IPC:  CLI commands travel over a UNIX domain socket at SOCKET_PATH.
 * Logging: bounded ring buffer + producer/consumer logger threads per container.
 */
 
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
 
#include <pthread.h>
#include <sched.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/ioctl.h>
 
#include "monitor_ioctl.h"
 
/* =========================================================================
 * Constants
 * ========================================================================= */
#define MAX_CONTAINERS  16
#define NAME_LEN        64
#define LOG_DIR         "/tmp/jackfruit_logs"
#define SOCKET_PATH     "/tmp/jackfruit.sock"
#define STACK_SIZE      (1024 * 1024)
 
#define LOG_BUF_SLOTS   256
#define LOG_BUF_LINE    512
 
/* =========================================================================
 * Container state
 * ========================================================================= */
typedef enum {
    ST_EMPTY = 0,
    ST_STARTING,
    ST_RUNNING,
    ST_STOPPED,
    ST_KILLED,
    ST_LIMIT_KILLED,
} ContainerState;
 
static const char *state_str(ContainerState s) {
    switch (s) {
        case ST_EMPTY:        return "empty";
        case ST_STARTING:     return "starting";
        case ST_RUNNING:      return "running";
        case ST_STOPPED:      return "stopped";
        case ST_KILLED:       return "killed";
        case ST_LIMIT_KILLED: return "limit_killed";
        default:              return "unknown";
    }
}
 
/* =========================================================================
 * Bounded ring buffer
 * ========================================================================= */
typedef struct {
    char            lines[LOG_BUF_SLOTS][LOG_BUF_LINE];
    int             head;
    int             tail;
    int             count;
    pthread_mutex_t lock;
    pthread_cond_t  not_empty;
    pthread_cond_t  not_full;
} LogBuffer;
 
static void logbuf_init(LogBuffer *b) {
    memset(b, 0, sizeof(*b));
    pthread_mutex_init(&b->lock, NULL);
    pthread_cond_init(&b->not_empty, NULL);
    pthread_cond_init(&b->not_full, NULL);
}
 
static void logbuf_push(LogBuffer *b, const char *line) {
    pthread_mutex_lock(&b->lock);
    while (b->count == LOG_BUF_SLOTS)
        pthread_cond_wait(&b->not_full, &b->lock);
    strncpy(b->lines[b->head], line, LOG_BUF_LINE - 1);
    b->head = (b->head + 1) % LOG_BUF_SLOTS;
    b->count++;
    pthread_cond_signal(&b->not_empty);
    pthread_mutex_unlock(&b->lock);
}
 
static int logbuf_pop(LogBuffer *b, char *out, int timeout_ms) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += timeout_ms * 1000000L;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
 
    pthread_mutex_lock(&b->lock);
    while (b->count == 0) {
        int rc = pthread_cond_timedwait(&b->not_empty, &b->lock, &ts);
        if (rc == ETIMEDOUT) { pthread_mutex_unlock(&b->lock); return 0; }
    }
    strncpy(out, b->lines[b->tail], LOG_BUF_LINE - 1);
    b->tail = (b->tail + 1) % LOG_BUF_SLOTS;
    b->count--;
    pthread_cond_signal(&b->not_full);
    pthread_mutex_unlock(&b->lock);
    return 1;
}
 
/* =========================================================================
 * Container descriptor
 * ========================================================================= */
typedef struct {
    int            slot;
    char           name[NAME_LEN];
    pid_t          host_pid;
    time_t         start_time;
    ContainerState state;
    long           soft_limit_kb;
    long           hard_limit_kb;
    char           log_path[256];
    int            exit_status;
 
    int            pipe_rd;
    int            pipe_wr;
    LogBuffer      logbuf;
    pthread_t      reader_tid;
    pthread_t      writer_tid;
    volatile int   log_done;
} Container;
 
/* =========================================================================
 * Global state
 * ========================================================================= */
static Container       g_containers[MAX_CONTAINERS];
static pthread_mutex_t g_table_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile int    g_running    = 1;
static int             g_monitor_fd = -1;
 
/* =========================================================================
 * Slot management
 * ========================================================================= */
static Container *alloc_slot(void) {
    int i;
    pthread_mutex_lock(&g_table_lock);
    for (i = 0; i < MAX_CONTAINERS; i++) {
        if (g_containers[i].state == ST_EMPTY) {
            memset(&g_containers[i], 0, sizeof(g_containers[i]));
            g_containers[i].slot  = i;
            g_containers[i].state = ST_STARTING;
            pthread_mutex_unlock(&g_table_lock);
            return &g_containers[i];
        }
    }
    pthread_mutex_unlock(&g_table_lock);
    return NULL;
}
 
static Container *find_by_name(const char *name) {
    int i;
    pthread_mutex_lock(&g_table_lock);
    for (i = 0; i < MAX_CONTAINERS; i++) {
        if (g_containers[i].state != ST_EMPTY &&
            strcmp(g_containers[i].name, name) == 0) {
            pthread_mutex_unlock(&g_table_lock);
            return &g_containers[i];
        }
    }
    pthread_mutex_unlock(&g_table_lock);
    return NULL;
}
 
static Container *find_by_pid(pid_t pid) {
    int i;
    for (i = 0; i < MAX_CONTAINERS; i++) {
        if (g_containers[i].host_pid == pid)
            return &g_containers[i];
    }
    return NULL;
}
 
/* =========================================================================
 * Logging threads
 * ========================================================================= */
static void *logger_reader(void *arg) {
    Container *c = (Container *)arg;
    char line[LOG_BUF_LINE];
    FILE *fp = fdopen(c->pipe_rd, "r");
    if (!fp) { perror("fdopen pipe"); return NULL; }
    while (!c->log_done) {
        if (!fgets(line, sizeof(line), fp)) break;
        logbuf_push(&c->logbuf, line);
    }
    while (fgets(line, sizeof(line), fp))
        logbuf_push(&c->logbuf, line);
    fclose(fp);
    return NULL;
}
 
static void *logger_writer(void *arg) {
    Container *c = (Container *)arg;
    char line[LOG_BUF_LINE];
    FILE *fp = fopen(c->log_path, "a");
    if (!fp) { perror("fopen log"); return NULL; }
    while (!c->log_done || c->logbuf.count > 0) {
        if (logbuf_pop(&c->logbuf, line, 200))
            fputs(line, fp);
    }
    fclose(fp);
    return NULL;
}
 
static void start_logger_threads(Container *c) {
    logbuf_init(&c->logbuf);
    c->log_done = 0;
    pthread_create(&c->reader_tid, NULL, logger_reader, c);
    pthread_create(&c->writer_tid, NULL, logger_writer, c);
}
 
static void stop_logger_threads(Container *c) {
    c->log_done = 1;
    pthread_cond_signal(&c->logbuf.not_empty);
    pthread_join(c->reader_tid, NULL);
    pthread_join(c->writer_tid, NULL);
}
 
/* =========================================================================
 * Container child (runs inside new namespaces)
 * ========================================================================= */
typedef struct {
    char rootfs[256];
    char cmd[512];
    char hostname[64];
    int  pipe_wr;
} ChildArgs;
 
static int container_child(void *arg) {
    ChildArgs *a = (ChildArgs *)arg;
    dup2(a->pipe_wr, STDOUT_FILENO);
    dup2(a->pipe_wr, STDERR_FILENO);
    close(a->pipe_wr);
 
    sethostname(a->hostname, strlen(a->hostname));
 
    if (chroot(a->rootfs) < 0) { perror("chroot"); return 1; }
    if (chdir("/") < 0)         { perror("chdir");  return 1; }
 
    mount("proc", "/proc", "proc", MS_NOEXEC | MS_NOSUID | MS_NODEV, NULL);
 
    char *argv[] = { "/bin/sh", "-c", a->cmd, NULL };
    execv("/bin/sh", argv);
    perror("execv");
    return 1;
}
 
/* =========================================================================
 * Launch a container
 * ========================================================================= */
static Container *launch_container(const char *name, const char *rootfs,
                                    const char *cmd,
                                    long soft_kb, long hard_kb) {
    Container *c = alloc_slot();
    char *stack, *stack_top;
    ChildArgs *args;
    int pipefd[2];
    int flags;
    pid_t pid;
 
    if (!c) { fprintf(stderr, "No free container slots\n"); return NULL; }
 
    strncpy(c->name, name, NAME_LEN - 1);
    c->soft_limit_kb = soft_kb;
    c->hard_limit_kb = hard_kb;
    c->start_time    = time(NULL);
 
    mkdir(LOG_DIR, 0755);
    snprintf(c->log_path, sizeof(c->log_path), "%s/%s.log", LOG_DIR, name);
 
    if (pipe(pipefd) < 0) { perror("pipe"); c->state = ST_EMPTY; return NULL; }
    c->pipe_rd = pipefd[0];
    c->pipe_wr = pipefd[1];
 
    args = malloc(sizeof(ChildArgs));
    strncpy(args->rootfs,   rootfs, sizeof(args->rootfs) - 1);
    strncpy(args->cmd,      cmd,    sizeof(args->cmd)    - 1);
    strncpy(args->hostname, name,   sizeof(args->hostname)- 1);
    args->pipe_wr = c->pipe_wr;
 
    stack = malloc(STACK_SIZE);
    if (!stack) { perror("malloc stack"); c->state = ST_EMPTY; return NULL; }
    stack_top = stack + STACK_SIZE;
 
    flags = SIGCHLD | CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS;
    pid = clone(container_child, stack_top, flags, args);
    if (pid < 0) {
        perror("clone");
        free(stack); free(args);
        c->state = ST_EMPTY;
        return NULL;
    }
 
    c->host_pid = pid;
    c->state    = ST_RUNNING;
    close(c->pipe_wr);
 
    start_logger_threads(c);
 
    if (g_monitor_fd >= 0) {
        struct container_info info;
        memset(&info, 0, sizeof(info));
        info.host_pid      = pid;
        info.soft_limit_kb = soft_kb;
        info.hard_limit_kb = hard_kb;
        strncpy(info.name, name, sizeof(info.name) - 1);
        if (ioctl(g_monitor_fd, MONITOR_REGISTER, &info) < 0)
            perror("ioctl MONITOR_REGISTER");
    }
 
    printf("[supervisor] Started container '%s' pid=%d soft=%ldKB hard=%ldKB\n",
           name, pid, soft_kb, hard_kb);
    free(stack);
    free(args);
    return c;
}
 
/* =========================================================================
 * Signal handlers
 * ========================================================================= */
static void sigchld_handler(int sig) {
    int status;
    pid_t pid;
    Container *c;
    (void)sig;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        c = find_by_pid(pid);
        if (!c) continue;
        c->exit_status = status;
        if (c->state == ST_RUNNING) c->state = ST_STOPPED;
        if (g_monitor_fd >= 0)
            ioctl(g_monitor_fd, MONITOR_UNREGISTER, &pid);
        stop_logger_threads(c);
        printf("[supervisor] Container '%s' (pid=%d) exited\n", c->name, pid);
    }
}
 
static void sigterm_handler(int sig) {
    (void)sig;
    g_running = 0;
}
 
/* =========================================================================
 * IPC command handlers
 * ========================================================================= */
#define CMD_BUF 1024
 
static void handle_ps(int fd) {
    int i;
    pthread_mutex_lock(&g_table_lock);
    dprintf(fd, "%-4s %-16s %-8s %-14s %-12s %-12s %s\n",
            "SLOT", "NAME", "PID", "STATE", "SOFT(KB)", "HARD(KB)", "STARTED");
    for (i = 0; i < MAX_CONTAINERS; i++) {
        Container *c = &g_containers[i];
        char tbuf[32];
        struct tm *tm;
        if (c->state == ST_EMPTY) continue;
        tm = localtime(&c->start_time);
        strftime(tbuf, sizeof(tbuf), "%H:%M:%S", tm);
        dprintf(fd, "%-4d %-16s %-8d %-14s %-12ld %-12ld %s\n",
                i, c->name, c->host_pid, state_str(c->state),
                c->soft_limit_kb, c->hard_limit_kb, tbuf);
    }
    pthread_mutex_unlock(&g_table_lock);
    dprintf(fd, "OK\n");
}
 
static void handle_logs(int fd, const char *name) {
    char line[512];
    FILE *fp;
    Container *c = find_by_name(name);
    if (!c) { dprintf(fd, "ERR no container named '%s'\n", name); return; }
    fp = fopen(c->log_path, "r");
    if (!fp) { dprintf(fd, "ERR cannot open log: %s\n", strerror(errno)); return; }
    while (fgets(line, sizeof(line), fp))
        write(fd, line, strlen(line));
    fclose(fp);
    dprintf(fd, "OK\n");
}
 
static void handle_stop(int fd, const char *name) {
    Container *c = find_by_name(name);
    if (!c) { dprintf(fd, "ERR no container named '%s'\n", name); return; }
    if (c->state != ST_RUNNING) {
        dprintf(fd, "ERR container '%s' is not running\n", name); return;
    }
    kill(c->host_pid, SIGTERM);
    c->state = ST_KILLED;
    dprintf(fd, "OK\n");
}
 
/*
 * Parse: <name> <rootfs> <cmd> [--soft KB] [--hard KB]
 * Returns 1 on success, 0 on failure.
 */
static int parse_start_args(const char *args, char *name, char *rootfs,
                             char *cmd, long *soft_kb, long *hard_kb) {
    char buf[CMD_BUF];
    char *p;
    char *soft_p, *hard_p;
 
    strncpy(buf, args, CMD_BUF - 1);
 
    /* Extract --soft and --hard before splitting positional args */
    *soft_kb = 0;
    *hard_kb = 0;
 
    soft_p = strstr(buf, "--soft ");
    if (soft_p) {
        *soft_kb = atol(soft_p + 7);
        /* blank out the flag so sscanf doesn't see it */
        memset(soft_p, ' ', strcspn(soft_p, " \t\n") + 1 +
               strcspn(soft_p + 7, " \t\n"));
    }
 
    hard_p = strstr(buf, "--hard ");
    if (hard_p) {
        *hard_kb = atol(hard_p + 7);
        memset(hard_p, ' ', strcspn(hard_p, " \t\n") + 1 +
               strcspn(hard_p + 7, " \t\n"));
    }
 
    /* Trim trailing spaces from buf for clean cmd parsing */
    p = buf + strlen(buf) - 1;
    while (p > buf && *p == ' ') *p-- = '\0';
 
    if (sscanf(buf, "%63s %255s %511[^\n]", name, rootfs, cmd) < 3)
        return 0;
    return 1;
}
 
static void handle_start(int fd, char *args) {
    char name[NAME_LEN], rootfs[256], cmd[512];
    long soft_kb, hard_kb;
    Container *c;
 
    if (!parse_start_args(args, name, rootfs, cmd, &soft_kb, &hard_kb)) {
        dprintf(fd, "ERR usage: start <name> <rootfs> <cmd> [--soft KB] [--hard KB]\n");
        return;
    }
    c = launch_container(name, rootfs, cmd, soft_kb, hard_kb);
    if (!c) { dprintf(fd, "ERR failed to start container\n"); return; }
    dprintf(fd, "Started '%s' pid=%d soft=%ldKB hard=%ldKB\nOK\n",
            name, c->host_pid, soft_kb, hard_kb);
}
 
static void handle_run(int fd, char *args) {
    char name[NAME_LEN], rootfs[256], cmd[512];
    long soft_kb, hard_kb;
    Container *c;
    int status;
    pid_t pid;
 
    if (!parse_start_args(args, name, rootfs, cmd, &soft_kb, &hard_kb)) {
        dprintf(fd, "ERR usage: run <name> <rootfs> <cmd> [--soft KB] [--hard KB]\n");
        return;
    }
    c = launch_container(name, rootfs, cmd, soft_kb, hard_kb);
    if (!c) { dprintf(fd, "ERR failed to start container\n"); return; }
 
    pid = c->host_pid;
    dprintf(fd, "Running '%s' pid=%d (waiting...)\n", name, pid);
    waitpid(pid, &status, 0);
    c->exit_status = status;
    c->state = ST_STOPPED;
    stop_logger_threads(c);
    dprintf(fd, "Container '%s' exited status=%d\nOK\n",
            name, WEXITSTATUS(status));
}
 
static void dispatch_command(int fd, char *line) {
    line[strcspn(line, "\r\n")] = '\0';
    if (strcmp(line, "ps") == 0)              handle_ps(fd);
    else if (strncmp(line, "logs ", 5) == 0)  handle_logs(fd, line + 5);
    else if (strncmp(line, "stop ", 5) == 0)  handle_stop(fd, line + 5);
    else if (strncmp(line, "start ", 6) == 0) handle_start(fd, line + 6);
    else if (strncmp(line, "run ", 4) == 0)   handle_run(fd, line + 4);
    else dprintf(fd, "ERR unknown command: %s\n", line);
}
 
/* =========================================================================
 * Supervisor main loop
 * ========================================================================= */
static void supervisor_loop(void) {
    int srv, cli;
    struct sigaction sa_chld, sa_term;
    struct sockaddr_un addr;
    char buf[CMD_BUF];
    ssize_t n;
    int i;
 
    sa_chld.sa_handler = sigchld_handler;
    sa_chld.sa_flags   = SA_RESTART | SA_NOCLDSTOP;
    sigemptyset(&sa_chld.sa_mask);
    sigaction(SIGCHLD, &sa_chld, NULL);
 
    sa_term.sa_handler = sigterm_handler;
    sa_term.sa_flags   = 0;
    sigemptyset(&sa_term.sa_mask);
    sigaction(SIGINT,  &sa_term, NULL);
    sigaction(SIGTERM, &sa_term, NULL);
 
    g_monitor_fd = open(MONITOR_DEVICE, O_RDWR);
    if (g_monitor_fd < 0)
        fprintf(stderr, "[supervisor] Monitor device unavailable (no kernel module?)\n");
    else
        fprintf(stderr, "[supervisor] Kernel monitor device opened\n");
 
    unlink(SOCKET_PATH);
    srv = socket(AF_UNIX, SOCK_STREAM, 0);
    if (srv < 0) { perror("socket"); exit(1); }
 
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);
    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); exit(1);
    }
    chmod(SOCKET_PATH, 0666);
    listen(srv, 8);
    fcntl(srv, F_SETFL, O_NONBLOCK);
 
    printf("[supervisor] Listening on %s\n", SOCKET_PATH);
 
    while (g_running) {
        cli = accept(srv, NULL, NULL);
        if (cli < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) { usleep(50000); continue; }
            if (errno == EINTR) continue;
            perror("accept"); break;
        }
        n = read(cli, buf, sizeof(buf) - 1);
        if (n > 0) { buf[n] = '\0'; dispatch_command(cli, buf); }
        close(cli);
    }
 
    printf("[supervisor] Shutting down...\n");
    pthread_mutex_lock(&g_table_lock);
    for (i = 0; i < MAX_CONTAINERS; i++) {
        if (g_containers[i].state == ST_RUNNING) {
            kill(g_containers[i].host_pid, SIGTERM);
            g_containers[i].state = ST_KILLED;
        }
    }
    pthread_mutex_unlock(&g_table_lock);
 
    sleep(1);
    while (waitpid(-1, NULL, WNOHANG) > 0) {}
    close(srv);
    unlink(SOCKET_PATH);
    if (g_monitor_fd >= 0) close(g_monitor_fd);
    printf("[supervisor] Done.\n");
}
 
/* =========================================================================
 * CLI client
 * ========================================================================= */
static void cli_send(const char *cmd) {
    int fd;
    struct sockaddr_un addr;
    char buf[4096];
    ssize_t n;
 
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); exit(1); }
 
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect (is supervisor running?)"); exit(1);
    }
 
    write(fd, cmd, strlen(cmd));
    write(fd, "\n", 1);
    while ((n = read(fd, buf, sizeof(buf))) > 0)
        fwrite(buf, 1, n, stdout);
    close(fd);
}
 
/* =========================================================================
 * main
 * ========================================================================= */
int main(int argc, char *argv[]) {
    const char *subcmd;
    char buf[CMD_BUF];
    int i;
 
    if (argc < 2) {
        fprintf(stderr,
            "Usage:\n"
            "  sudo ./engine supervisor <rootfs>\n"
            "  sudo ./engine start <name> <rootfs> <cmd> [--soft KB] [--hard KB]\n"
            "  sudo ./engine run   <name> <rootfs> <cmd> [--soft KB] [--hard KB]\n"
            "  sudo ./engine ps\n"
            "  sudo ./engine logs  <name>\n"
            "  sudo ./engine stop  <name>\n");
        return 1;
    }
 
    subcmd = argv[1];
 
    if (strcmp(subcmd, "supervisor") == 0) {
        supervisor_loop();
 
    } else if (strcmp(subcmd, "ps") == 0) {
        cli_send("ps");
 
    } else if (strcmp(subcmd, "logs") == 0 && argc >= 3) {
        snprintf(buf, sizeof(buf), "logs %s", argv[2]);
        cli_send(buf);
 
    } else if (strcmp(subcmd, "stop") == 0 && argc >= 3) {
        snprintf(buf, sizeof(buf), "stop %s", argv[2]);
        cli_send(buf);
 
    } else if ((strcmp(subcmd, "start") == 0 || strcmp(subcmd, "run") == 0)
               && argc >= 5) {
        /* Build: <name> <rootfs> <cmd words...> [--soft X] [--hard X] */
        char cmd[512] = "";
        for (i = 4; i < argc; i++) {
            if (i > 4) strncat(cmd, " ", sizeof(cmd) - strlen(cmd) - 1);
            strncat(cmd, argv[i], sizeof(cmd) - strlen(cmd) - 1);
        }
        snprintf(buf, sizeof(buf), "%s %s %s %s", subcmd, argv[2], argv[3], cmd);
        cli_send(buf);
 
    } else {
        fprintf(stderr, "Unknown command or missing arguments: %s\n", subcmd);
        return 1;
    }
 
    return 0;
}
