# ===========================================================================
#  OS-Jackfruit — Multi-Container Runtime
#  Builds: engine (user-space), monitor.ko (kernel module), workloads
# ===========================================================================

CC        = gcc
CFLAGS    = -Wall -Wextra -g -pthread
KDIR      = /lib/modules/$(shell uname -r)/build

# ---------------------------------------------------------------------------
# Default target — build everything
# ---------------------------------------------------------------------------
.PHONY: all
all: engine workloads monitor_module

# ---------------------------------------------------------------------------
# User-space supervisor
# ---------------------------------------------------------------------------
engine: engine.c monitor_ioctl.h
	$(CC) $(CFLAGS) -o engine engine.c

# ---------------------------------------------------------------------------
# Workload binaries (copied into rootfs before container launch)
# ---------------------------------------------------------------------------
.PHONY: workloads
workloads: workloads/cpu_burn workloads/io_stress

workloads/cpu_burn: workloads/cpu_burn.c
	$(CC) $(CFLAGS) -o workloads/cpu_burn workloads/cpu_burn.c -lm

workloads/io_stress: workloads/io_stress.c
	$(CC) $(CFLAGS) -o workloads/io_stress workloads/io_stress.c

# ---------------------------------------------------------------------------
# Kernel module
# ---------------------------------------------------------------------------
.PHONY: monitor_module
monitor_module:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

# Kbuild picks up this variable to know which source files form the module
obj-m += monitor.o

# ---------------------------------------------------------------------------
# Install workloads into rootfs so containers can run them
# ---------------------------------------------------------------------------
.PHONY: install-workloads
install-workloads: workloads
	cp workloads/cpu_burn  rootfs/
	cp workloads/io_stress rootfs/
	@echo "Workloads copied into rootfs/"

# ---------------------------------------------------------------------------
# Load / unload helpers  (require sudo)
# ---------------------------------------------------------------------------
.PHONY: load
load:
	sudo insmod monitor.ko
	@echo "Module loaded. Device: $$(ls -l /dev/container_monitor)"

.PHONY: unload
unload:
	sudo rmmod monitor
	@echo "Module unloaded."

# ---------------------------------------------------------------------------
# Clean
# ---------------------------------------------------------------------------
.PHONY: clean
clean:
	rm -f engine
	rm -f workloads/cpu_burn workloads/io_stress
	# Kernel module artifacts
	$(MAKE) -C $(KDIR) M=$(PWD) clean 2>/dev/null || true

# ---------------------------------------------------------------------------
# Help
# ---------------------------------------------------------------------------
.PHONY: help
help:
	@echo ""
	@echo "  make               — build everything"
	@echo "  make engine        — build user-space supervisor only"
	@echo "  make workloads     — build cpu_burn and io_stress"
	@echo "  make monitor_module— build the kernel module"
	@echo "  make install-workloads — copy workloads into rootfs/"
	@echo "  make load          — sudo insmod monitor.ko"
	@echo "  make unload        — sudo rmmod monitor"
	@echo "  make clean         — remove all build artifacts"
	@echo ""
