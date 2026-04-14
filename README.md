
## 1. Team Information

| Name | SRN |
|------|-----|
| Sushanth S Rao | PES1UG24CS485 |
| Swaroop V  | PES1UG24CS923 |

---


## 2. Build, Load, and Run Instructions

Step-by-step commands to build the project, load the kernel module, and start the supervisor. These are complete enough to reproduce the setup from scratch on a fresh Ubuntu VM.

### Prerequisites

Ubuntu 22.04 or 24.04 VM with Secure Boot OFF. WSL is not supported.

```bash
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r) git
````

### Clone and build

```bash
git clone [https://github.com/](https://github.com/)<your-username>/OS-Jackfruit.git
cd OS-Jackfruit/boilerplate
make
```

This produces: `engine`, `memory_hog`, `cpu_hog`, `io_pulse`, and `monitor.ko`

### Prepare base rootfs

*(Note: If you haven't downloaded Alpine yet)*

```bash
mkdir rootfs-base
wget [https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz](https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz)
tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs-base
cp cpu_hog memory_hog io_pulse rootfs-base/
```

### Load kernel module

```bash
sudo insmod monitor.ko
```

### Verify control device

```bash
ls -l /dev/container_monitor
```

### Start supervisor

```bash
sudo ./engine supervisor ./rootfs-base
```

### Create per-container writable rootfs copies

```bash
cp -a ./rootfs-base ./rootfs-alpha
cp -a ./rootfs-base ./rootfs-beta
```

### In another terminal: start two containers

```bash
sudo ./engine start alpha ./rootfs-alpha "/bin/sleep 100" --soft-mib 48 --hard-mib 80
sudo ./engine start beta ./rootfs-beta "/bin/sleep 100" --soft-mib 64 --hard-mib 96
```

### List tracked containers

```bash
sudo ./engine ps
```

### Inspect one container

```bash
sudo ./engine logs alpha
```

### Run memory test inside a container

*(The test program `memory_hog` is already copied into the rootfs)*

```bash
sudo ./engine start memtest ./rootfs-alpha "/memory_hog" --soft-mib 40 --hard-mib 60
```

### Run scheduling experiment workloads

*(Compare observed behavior using `top` in another terminal)*

```bash
sudo ./engine start c1 ./rootfs-alpha "/cpu_hog" --nice -20
sudo ./engine start c2 ./rootfs-beta "/cpu_hog" --nice 19
```

### Stop containers

```bash
sudo ./engine stop alpha
sudo ./engine stop beta
sudo ./engine stop memtest
sudo ./engine stop c1
sudo ./engine stop c2
```

### Stop supervisor

Press `Ctrl+C` in the terminal where the supervisor is running to trigger a clean exit.

### Inspect kernel logs

```bash
dmesg | tail -n 20
```

### Unload module

```bash
sudo rmmod monitor
```

-----

## 3\. Demo with Screenshots



### 1\. Multi-container supervision

> **Demonstration:** Two containers (`alpha` and `beta`) are running simultaneously under a single supervisor process.

<img width="2456" height="320" alt="image" src="https://github.com/user-attachments/assets/048285ac-c7bc-472c-ac38-d92861bf0d4c" />

### 2\. Metadata tracking

> **Demonstration:** The `engine ps` command displays the supervisor's internal metadata table, actively tracking each container's ID, host PID, State, and assigned Memory Limits.

<img width="1517" height="986" alt="image" src="https://github.com/user-attachments/assets/9d43fd38-e16c-44cf-9cc9-a439b8117744" />

### 3\. Bounded-buffer logging

> **Demonstration:** The supervisor successfully captures stdout from the container via a pipe, routes it through producer/consumer threads using a bounded buffer, and streams the log file contents back to the user upon running `engine logs`.

<img width="1911" height="1055" alt="image" src="https://github.com/user-attachments/assets/829346c7-0796-4f2d-91d9-9e63ab7fbb51" />

### 4\. CLI and IPC

> **Demonstration:** The CLI issues a `stop` command over the UNIX domain socket (`/tmp/mini_runtime.sock`). The supervisor successfully receives the IPC control request, kills the container, and sends a status response back to the CLI.

<img width="1255" height="229" alt="image" src="https://github.com/user-attachments/assets/5d25edae-b0cf-4d0e-a589-ddb2286f4af9" />

<img width="1439" height="280" alt="image" src="https://github.com/user-attachments/assets/2dac96d9-465c-4f3d-851a-614318c4ab94" />

### 5\. Soft-limit warning

> **Demonstration:** The `dmesg` output shows the custom kernel module (`monitor.ko`) logging a `SOFT LIMIT` warning when the container's RSS memory usage exceeds the configured threshold.

### 6\. Hard-limit enforcement

> **Demonstration:** `dmesg` shows the kernel module sending `SIGKILL` after the hard limit is breached. The subsequent `engine ps` output confirms the supervisor caught the signal and properly updated the container's metadata state to `hard_limit_killed`.

### 7\. Scheduling experiment

> **Demonstration:** The `top` output shows two competing `cpu_hog` workloads. The Linux Completely Fair Scheduler (CFS) allocates significantly more CPU time (100%) to the container with a High Priority (`NI -20`) compared to the Low Priority container (`NI 19`, 75% CPU).

### 8\. Clean teardown

> **Demonstration:** `ps aux | grep defunct` proves no zombie processes are left behind (successful `waitpid` reaping). The supervisor cleanly drains the bounded buffer, joins all threads, and exits cleanly upon receiving `SIGINT`.

-----

## 4\. Engineering Analysis

### 4.1 Isolation Mechanisms

We achieved isolation using `clone()` with `CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS`.

  * **PID namespaces** ensure processes inside the container cannot see or interact with host processes.
  * **UTS namespaces** allow custom hostnames.
  * **Mount namespaces**, combined with `chroot()`, lock the container to its specific `rootfs` directory. Network and IPC namespaces were left shared with the host as per the requirements.

### 4.2 Supervisor Lifecycle and IPC

The supervisor acts as the `init` process for the containers. It is essential for preventing zombie processes by catching `SIGCHLD` and calling `waitpid()`. The CLI communicates with the supervisor via a UNIX Domain Socket (`SOCK_STREAM`). We utilized a `select()` event loop to handle non-blocking CLI requests without stalling child process reaping.

### 4.3 Thread Synchronization (Bounded Buffer)

Standard output from containers is captured via pipes. We implemented a thread-safe bounded buffer using `pthread_mutex_t` and `pthread_cond_t`. Producer threads push log chunks into the buffer, while a single consumer thread safely writes them to disk. The condition variables prevent busy-waiting, ensuring threads block efficiently when the buffer is full or empty.

### 4.4 Kernel Memory Monitoring

Memory enforcement was moved to kernel space (via an LKM) because user-space polling is subject to scheduler delays. The module uses a timer (`timer_setup`) to check the RSS (`get_mm_rss`) of registered PIDs every second. It logs `KERN_WARNING` for soft limits and directly issues `send_sig(SIGKILL)` for hard limits, guaranteeing enforcement even under heavy system load.
