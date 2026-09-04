# FreeDot Linux

A reproducible, minimal Linux distribution and custom C++ init system built from the ground up to understand how Linux actually boots, manages processes, and brings up userspace without systemd bloat.

---

## Why Build This

Most modern Linux distros boot into gigabytes of background services, complex D-Bus IPC layers, and tens of thousands of configuration lines. Tbh I wanted to know what happens underneath all that noise.

FreeDot Linux is a ground-up systems project exploring:
- Kernel bootstrapping with an upstream Linux LTS kernel.
- Userspace initialization as PID 1 (`/init`) written in clean C++20.
- Low-level networking without relying on external utilities like `ifconfig` or `ip`.
- Process supervision, signal handling, and zombie reaping.
- Inter-process communication via UNIX domain stream sockets.

---

## Strict Hardware Constraints & Targets

Everything in this project is engineered to run under tight memory and disk budgets:
- **Target Disk Image**: <= 600 MB
- **Target Idle RAM**: <= 600 MB (runs comfortably in a 512 MB QEMU virtual machine)
- **Target Architecture**: x86_64
- **Host Development RAM**: 2 GB constraint
- **Kernel**: Upstream Linux LTS
- **Init System**: Custom FreeDot C++20 PID 1 binary

---

## Architecture Breakdown

### 1. Custom Init System (`src/init.cpp`)
When the Linux kernel finishes loading hardware drivers, it launches the first userspace process: PID 1. FreeDot Init replaces traditional init systems with a minimal, single-binary C++ supervisor:
- **Virtual Filesystem Mounting**: Mounts `/proc` (procfs), `/sys` (sysfs), `/dev` (devtmpfs), and `/run` (tmpfs) immediately upon boot.
- **Zombie Process Reaping**: Hooks `SIGCHLD` and calls non-blocking `waitpid(-1, &status, WNOHANG)` to reap terminated child processes so PID tables never leak.
- **Kernel Socket Networking**: Configures network interfaces (`lo` at `127.0.0.1/8` and `eth0` at `10.0.2.15/24`) directly through Linux `ioctl` socket calls (`SIOCSIFADDR`, `SIOCSIFNETMASK`, `SIOCSIFFLAGS`).
- **Process Supervision**: Parses `/etc/freedot.d/*.conf` unit files, spawns daemons and interactive TTY shells (attaching `/dev/ttyS0` or `/dev/console` with `setsid` and `dup2`), and auto-respawns crashed processes.
- **IPC Socket Server**: Listens on a non-blocking UNIX domain stream socket at `/run/freedot.sock`.
- **Graceful Shutdown**: Catches shutdown signals, politely notifies processes with `SIGTERM`, forces stragglers down with `SIGKILL`, flushes dirty file caches with `sync()`, unmounts virtual filesystems, and calls the Linux `reboot()` syscall.

### 2. Management CLI (`src/freedotctl.cpp`)
A lightweight client utility that talks directly to PID 1 over the `/run/freedot.sock` UNIX domain socket:
```bash
# Check running services and active PIDs
freedotctl status

# Restart a specific service
freedotctl restart <service_name>

# Reload service unit files from /etc/freedot.d/
freedotctl reload

# Cleanly unmount filesystems and power down
freedotctl poweroff

# Cleanly unmount filesystems and reboot
freedotctl reboot
```

### 3. System Metrics Daemon (`src/statsd.cpp`)
A minimal telemetry background daemon that queries kernel health via the `sysinfo()` syscall every 5 seconds. Records uptime, free/total RAM, and process counts to `/var/log/stats.log`.

---

## Repository Structure

```text
freedot-linux/
├── src/
│   ├── init.cpp           # Custom C++20 PID 1 init system & process supervisor
│   ├── freedotctl.cpp     # CLI client utility for interacting with PID 1
│   ├── statsd.cpp         # Lightweight system metrics logging daemon
│   └── init_test.cpp      # Standalone userspace syscall verification test
├── scripts/
│   ├── build_rootfs.sh    # Packs rootfs into a bootable initramfs.cpio.gz
│   └── run_qemu.sh        # Boots kernel and initramfs inside QEMU
├── .gitignore             # Build artifact exclusions
└── README.md              # Project documentation
```

---

## Build & Test Workflow

### 1. Compile the Userspace Binaries
Compile the init binary statically so it has zero external shared library dependencies:
```bash
g++ -std=c++20 -static -O2 src/init.cpp -o build/rootfs/init
g++ -std=c++20 -static -O2 src/freedotctl.cpp -o build/rootfs/bin/freedotctl
g++ -std=c++20 -static -O2 src/statsd.cpp -o build/rootfs/bin/statsd
```

### 2. Pack the Root Filesystem
Generate the compressed initramfs archive:
```bash
./scripts/build_rootfs.sh
```
This packages `build/rootfs` into `build/initramfs.cpio.gz`.

### 3. Boot with QEMU
Run the virtual machine with 512 MB RAM and serial console redirection:
```bash
./scripts/run_qemu.sh
```

To exit QEMU at any point, press `Ctrl + A` then `X`, or execute `freedotctl poweroff` inside the running machine.

---

## License

MIT License. Free for open-source systems exploration, education, and experimentation.
