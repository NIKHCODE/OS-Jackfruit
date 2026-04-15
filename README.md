# OS-Jackfruit — Multi-Container Runtime

**Team Name:** Dracarys

| Name | SRN |
|------|-----|
| M Nikhil Sai | PES1UG24CS253 |
| Manasi Manoj Jadhav | PES1UG24CS259 |

---

## 1. Team Information

**Team Name:** Dracarys  
**Members:**
- M Nikhil Sai — PES1UG24CS253
- Manasi Manoj Jadhav — PES1UG24CS259

---

## 2. Build, Load, and Run Instructions

### Prerequisites

Ubuntu 22.04 or 24.04 in a VM with Secure Boot OFF.

```bash
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r)
```

### Setup Alpine rootfs

```bash
mkdir -p ~/OS-Jackfruit/rootfs
cd ~/OS-Jackfruit
wget --no-check-certificate \
  "https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz"
tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs
rm alpine-minirootfs-3.20.3-x86_64.tar.gz
```

### Build everything

```bash
cd ~/OS-Jackfruit
make engine        # build user-space supervisor
make workloads     # build cpu_burn and io_stress
make monitor_module  # build kernel module
```

### Build and install workloads into rootfs

```bash
gcc -static -o rootfs/cpu_burn workloads/cpu_burn.c -lm
gcc -static -o rootfs/io_stress workloads/io_stress.c
gcc -static -o rootfs/mem_hog workloads/mem_hog.c
```

### Load kernel module

```bash
sudo insmod monitor.ko
# Verify device exists
ls -l /dev/container_monitor
# Verify in dmesg
dmesg | tail -3
```

### Start the supervisor (Terminal 1)

```bash
sudo ./engine supervisor ./rootfs
```

### Launch containers (Terminal 2)

```bash
# Start a container in the background
sudo ./engine start alpha ./rootfs "echo hello from container"

# Start a container with memory limits (soft=5MB, hard=20MB)
sudo ./engine start hog ./rootfs "/mem_hog" --soft 5120 --hard 20480

# Start a container and wait for it
sudo ./engine run beta ./rootfs "/cpu_burn 10"

# List all containers
sudo ./engine ps

# View container logs
sudo ./engine logs alpha

# Stop a container
sudo ./engine stop alpha
```

### Run scheduling experiments

```bash
# Experiment 1: Two CPU-bound containers at different priorities
sudo ./engine start hi_pri ./rootfs "/cpu_burn 60"
sudo nice -n 10 ./engine start lo_pri ./rootfs "/cpu_burn 60"
sudo ./engine ps

# Experiment 2: CPU-bound vs I/O-bound
sudo ./engine start cpu_c ./rootfs "/cpu_burn 30"
sudo ./engine start io_c ./rootfs "/io_stress 30"
sudo ./engine ps
```

### Monitor memory limits (Terminal 3)

```bash
dmesg -w | grep jackfruit
```

### Clean up

```bash
# Stop supervisor (Ctrl+C in Terminal 1), then:
sudo rmmod monitor
# Verify no zombies
ps aux | grep engine
```

---

## 3. Demo with Screenshots

### Screenshot 1 — Multi-container supervision
Two containers (hi3b, lo3b) running concurrently under one supervisor process, shown in `ps` output with slot, name, PID, state, and start time.



### Screenshot 2 — Metadata tracking
Output of `sudo ./engine ps` showing all tracked containers with their slot, name, host PID, state (running/stopped/killed), soft/hard memory limits, and start timestamp.

### Screenshot 3 — Bounded-buffer logging
Output of `sudo ./engine logs hi3` and `sudo ./engine logs lo3` showing container stdout captured through the pipe → ring buffer → log file pipeline.

### Screenshot 4 — CLI and IPC
`sudo ./engine start hog ./rootfs "/mem_hog" --soft 5120 --hard 20480` issued from CLI client, traveling over UNIX domain socket to supervisor, which responds with `Started 'hog' pid=2809 soft=5120KB hard=20480KB OK`.

### Screenshot 5 — Soft-limit warning
`dmesg` output showing:
```
jackfruit: SOFT LIMIT 'hog' (pid=2809) rss=5628KB > 5120KB - warning
```

### Screenshot 6 — Hard-limit enforcement
`dmesg` output showing:
```
jackfruit: HARD LIMIT 'hog' (pid=2809) rss=21204KB > 20480KB - SIGKILL
```
Container state transitions to `limit_killed` in supervisor metadata.

### Screenshot 7 — Scheduling experiment
`ps` output showing hi3b (nice 0) and lo3b (nice 10) running simultaneously. Logs show both ran for 120 seconds but produced different floating-point results, demonstrating different CPU share allocation by the Linux CFS scheduler.

### Screenshot 8 — Clean teardown
Supervisor prints `Shutting down... Done` after Ctrl+C. `ps aux | grep engine` shows no lingering supervisor or container processes — only `ibus-engine-simple` (a system process unrelated to our runtime) and the grep itself.

---

## 4. Engineering Analysis

### 4.1 Isolation Mechanisms

Our runtime achieves isolation by passing three flags to `clone()`: `CLONE_NEWPID`, `CLONE_NEWUTS`, and `CLONE_NEWNS`. Each flag asks the Linux kernel to create a new namespace of that type for the child process.

`CLONE_NEWPID` gives the container its own PID namespace. The first process inside the container sees itself as PID 1, even though the host kernel assigns it a different PID (e.g. 26493). This prevents containers from seeing or sending signals to host processes. `CLONE_NEWUTS` gives the container its own hostname, set via `sethostname()` to the container name. `CLONE_NEWNS` gives the container its own mount namespace, allowing us to mount `/proc` inside the container without affecting the host's `/proc`.

After `clone()`, the child calls `chroot()` into the Alpine rootfs. This changes the process's view of the filesystem root — it cannot access files outside the rootfs directory. The host kernel still underlies all containers: the same kernel handles all system calls, the same scheduler runs all threads, and the same physical memory is shared. Namespaces are a partitioning mechanism on top of a shared kernel, not full virtualisation.

### 4.2 Supervisor and Process Lifecycle

A long-running parent supervisor is necessary because Linux's process model ties child reaping to the parent. When a child exits, it becomes a zombie until the parent calls `waitpid()`. Without a persistent supervisor, orphaned containers would accumulate zombie entries in the process table.

The supervisor uses `SIGCHLD` with `SA_RESTART | SA_NOCLDSTOP` to be notified when any child exits. The handler calls `waitpid(-1, &status, WNOHANG)` in a loop to reap all available children without blocking. It then updates the container's state in the global table and stops its logger threads.

Container metadata (name, PID, state, limits, log path, exit status) is stored in a fixed-size array of `Container` structs protected by `g_table_lock`. The supervisor distinguishes three termination paths: graceful stop (SIGTERM from CLI → state `killed`), natural exit (process exits on its own → state `stopped`), and kernel hard-limit kill (SIGKILL from monitor → state `limit_killed`).

### 4.3 IPC, Threads, and Synchronization

The project uses two IPC mechanisms. The log pipeline uses pipes: each container's stdout and stderr are redirected into the write end of a pipe via `dup2()`. The supervisor holds the read end. A per-container `logger_reader` thread reads lines from the pipe and pushes them into a bounded ring buffer. A per-container `logger_writer` thread pops lines from the ring buffer and writes them to a log file on disk.

The ring buffer has `LOG_BUF_SLOTS = 256` slots. It is protected by a mutex with two condition variables: `not_empty` (writer waits here when buffer is empty) and `not_full` (reader waits here when buffer is full). Without the mutex, concurrent push and pop operations would corrupt the head/tail indices. Without the condition variables, threads would busy-spin wasting CPU. This is the classic bounded producer-consumer pattern.

The CLI control channel uses a UNIX domain socket at `/tmp/jackfruit.sock`. This is a different IPC mechanism from the pipe, as required. The supervisor listens in non-blocking mode; CLI clients connect, send a text command, and read the response. Using a socket rather than a second pipe allows multiple CLI clients to connect independently.

Shared metadata (the `g_containers` array) is protected separately by `g_table_lock`. This avoids a race condition where the SIGCHLD handler and a CLI command handler both modify the same container entry simultaneously.

### 4.4 Memory Management and Enforcement

RSS (Resident Set Size) measures the number of physical memory pages currently mapped and present in RAM for a process. It does not measure memory that has been allocated but not yet touched (lazy allocation), memory that has been swapped out, or memory-mapped files that have not been faulted in. RSS is read from the kernel's `mm_struct` via `get_mm_rss()` in the monitor module.

Soft and hard limits serve different policy purposes. A soft limit is a warning threshold — it tells the operator that a container is using more memory than expected, but allows it to continue running. This is useful for alerting without disrupting service. A hard limit is an enforcement threshold — when exceeded, the kernel module sends `SIGKILL` immediately. There is no way for the process to catch or ignore `SIGKILL`.

Memory enforcement belongs in kernel space for two reasons. First, a user-space monitor can be fooled or delayed — the monitored process could allocate and use memory faster than the monitor polls. Second, sending `SIGKILL` to a process in a different PID namespace requires the host PID, which user space has, but the kill must be authoritative. Kernel space has direct access to the task's `mm_struct` and can deliver signals atomically without race conditions.

### 4.5 Scheduling Behavior

Linux uses the Completely Fair Scheduler (CFS) for normal processes. CFS assigns CPU time proportional to each process's weight, which is derived from its nice value. A nice value of 0 has weight 1024; a nice value of 10 has weight 110. This means a process at nice 0 receives roughly 9x more CPU time than one at nice 10 when both are runnable.

In our Experiment 1, `hi_pri` (nice 0) and `lo_pri` (nice 10) both ran `cpu_burn` for 30 seconds. `hi_pri` completed and exited before `lo_pri` even started in several runs, because the supervisor process itself (which launches containers) was running at higher priority. In the concurrent run (hi3b vs lo3b, 120 seconds each), both ran for the full duration but produced different result values, showing that lo3b performed fewer floating-point iterations per second due to receiving less CPU time from CFS.

In Experiment 2, `cpu_c` (CPU-bound) and `io_c` (I/O-bound) ran concurrently. The CPU-bound process kept its CPU core busy continuously. The I/O-bound process voluntarily gave up the CPU on every file write/read, entering a blocked state. CFS detected that `io_c` was not using its full time slice and boosted its scheduling priority slightly when it woke up, which is the scheduler's responsiveness heuristic for interactive/IO workloads. The CPU-bound container got consistent throughput; the I/O-bound container got lower latency on wakeup.

---

## 5. Design Decisions and Tradeoffs

### Namespace Isolation
**Choice:** Used `clone()` with `CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS` plus `chroot()`.  
**Tradeoff:** We do not use `CLONE_NEWNET` or `CLONE_NEWUSER`, so containers share the host network stack and run as root on the host.  
**Justification:** The project scope focuses on PID, filesystem, and hostname isolation. Adding network namespaces would require setting up virtual ethernet pairs (veth) which is significant additional complexity beyond the project requirements.

### Supervisor Architecture
**Choice:** Single long-running supervisor process with a fixed array of 16 container slots.  
**Tradeoff:** Maximum 16 concurrent containers; metadata is lost on supervisor restart.  
**Justification:** A fixed array avoids dynamic allocation complexity and is sufficient for a demonstration runtime. A production system would use a database-backed store for persistence.

### IPC and Logging
**Choice:** UNIX domain socket for CLI control; pipes + ring buffer for logging.  
**Tradeoff:** The ring buffer blocks the reader if full, which could slow container output if the writer thread falls behind.  
**Justification:** Blocking on full is safer than dropping log lines. The 256-slot buffer provides enough headroom for typical workloads. UNIX sockets were chosen over FIFOs because they support multiple concurrent clients naturally.

### Kernel Monitor
**Choice:** Misc device with `ioctl` for registration; kernel timer for periodic RSS checks.  
**Tradeoff:** 1-second polling granularity means a process could exceed its hard limit by a significant amount before being killed.  
**Justification:** A 1-second interval is a reasonable tradeoff between responsiveness and kernel overhead. Finer granularity (e.g. 100ms) would increase timer interrupt frequency. A more sophisticated approach would use memory cgroups, but that is outside the project scope.

### Scheduling Experiments
**Choice:** Used `nice` values to differentiate container priorities; measured via log output timestamps and floating-point result magnitude.  
**Tradeoff:** Nice values affect the entire supervisor process tree, not just the container child. CPU affinity (`taskset`) would give more precise control.  
**Justification:** `nice` is the simplest standard Linux mechanism for demonstrating CFS weight-based scheduling without requiring cgroup setup.

---

## 6. Scheduler Experiment Results

### Experiment 1: CPU-bound containers at different nice values

Both containers ran `cpu_burn` for 60 seconds concurrently.

| Container | Nice Value | Duration | Observation |
|-----------|-----------|----------|-------------|
| hi3 | 0 (normal) | 60s | Completed, higher result value |
| lo3 | 10 (lower priority) | 60s | Completed, lower result value |

**Result:** Both containers completed their 60-second duration (the timer is wall-clock based), but the floating-point accumulator result for `lo3` was consistently lower than `hi3`, indicating fewer CPU cycles were allocated to `lo3` per second. This is consistent with CFS weight ratio: nice 0 (weight 1024) vs nice 10 (weight 110) gives roughly a 9:1 CPU time ratio.

### Experiment 2: CPU-bound vs I/O-bound at same priority

Both containers ran for 30 seconds concurrently at the same nice value.

| Container | Type | Duration | Observation |
|-----------|------|----------|-------------|
| cpu_c | CPU-bound (cpu_burn) | 30s | Kept CPU busy 100% continuously |
| io_c | I/O-bound (io_stress) | 30s | Frequently blocked on disk I/O |

**Result:** The CPU-bound container saturated one CPU core. The I/O-bound container spent most of its time in the blocked state waiting for disk reads/writes to complete. CFS gave the I/O-bound container a scheduling boost on each wakeup (since it had not consumed its full time slice), resulting in lower latency for I/O operations. The CPU-bound container achieved higher raw throughput but received no such boost.

**Conclusion:** Linux CFS is fair in terms of CPU time allocation by weight, but also implements a responsiveness heuristic that benefits I/O-bound processes. This is the expected behavior for a general-purpose scheduler balancing throughput and interactivity.

---

## Repository Structure

```
OS-Jackfruit/
├── Makefile
├── README.md
├── engine.c               # user-space supervisor
├── monitor.c              # kernel module (LKM)
├── monitor_ioctl.h        # shared ioctl definitions
├── workloads/
│   ├── cpu_burn.c         # CPU-bound workload
│   ├── io_stress.c        # I/O-bound workload
│   └── mem_hog.c          # memory allocation workload
└── rootfs/                # Alpine mini rootfs (not committed)
```
