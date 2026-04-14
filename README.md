
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
git clone https://github.com/switchblade-kratos/OS-Jackfruit-Project.git
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

<img width="2219" height="224" alt="image" src="https://github.com/user-attachments/assets/50c84926-9406-4b09-80f6-70eb70378cc2" />

### 6\. Hard-limit enforcement

> **Demonstration:** `dmesg` shows the kernel module sending `SIGKILL` after the hard limit is breached. The subsequent `engine ps` output confirms the supervisor caught the signal and properly updated the container's metadata state to `hard_limit_killed`.

<img width="1465" height="314" alt="image" src="https://github.com/user-attachments/assets/913a3480-a28e-4723-86a4-43b22f9b5bf2" />

### 7\. Scheduling experiment

> **Demonstration:** The `top` output shows two competing `cpu_hog` workloads. The Linux Completely Fair Scheduler (CFS) allocates significantly more CPU time (100%) to the container with a High Priority (`NI -20`) compared to the Low Priority container (`NI 19`, 75% CPU).

<img width="1881" height="171" alt="image" src="https://github.com/user-attachments/assets/eb7631f8-185c-48a5-a5d1-5cafc94dd068" />

### 8\. Clean teardown

> **Demonstration:** `ps aux | grep defunct` proves no zombie processes are left behind (successful `waitpid` reaping). The supervisor cleanly drains the bounded buffer, joins all threads, and exits cleanly upon receiving `SIGINT`.

<img width="711" height="88" alt="image" src="https://github.com/user-attachments/assets/0b59c3cd-4dbc-49ba-9bc0-063919b5de16" />

<img width="1983" height="148" alt="image" src="https://github.com/user-attachments/assets/97da7c46-83fe-461f-900b-bef0263668a5" />

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

## 5. Design Decisions and Tradeoffs

| Subsystem | Design Choice | Concrete Tradeoff | Justification |
|-----------|---------------|-------------------|---------------|
| **Namespace Isolation** | Used `clone()` with `CLONE_NEWPID`, `CLONE_NEWUTS`, and `CLONE_NEWNS` combined with `chroot()`. | **Security vs. Simplicity:** We chose `chroot()` over `pivot_root()`. While `chroot` is easier to implement, it is technically possible for a root process to "break out" of a chroot jail. | It perfectly meets the project specification for isolation without the extreme configuration complexity of setting up nested mount points required by `pivot_root`. |
| **Supervisor Architecture** | A long-running parent daemon using a `select()` event loop to handle concurrent CLI requests. | **Resource Overhead:** Keeping a supervisor process alive for the entire lifecycle of all containers consumes a small but constant amount of host memory and a PID slot. | This is the only way to reliably reap zombie processes (`waitpid`) and maintain a persistent logging pipeline that survives even if a container crashes. |
| **IPC and Logging** | UNIX Domain Sockets for control commands and a Bounded Buffer (Mutex + CV) for log streaming. | **Memory vs. Reliability:** The bounded buffer has a fixed capacity (64 slots). If the container produces logs faster than the consumer can write them, the producer threads will block. | This ensures "Backpressure." It prevents a runaway container from flooding the host's memory with unwritten log strings, protecting system stability. |
| **Kernel Monitor** | A Linux Kernel Module (LKM) using `timer_setup` to poll process RSS (Resident Set Size). | **CPU Interrupts:** Checking memory every 1 second in kernel space adds a tiny amount of overhead to the CPU scheduler's interrupt handling. | User-space monitors can be swapped out or delayed by the scheduler. A kernel-level monitor is "omnipresent" and guarantees that a hard limit kill happens even under 100% CPU load. |
| **Scheduling Experiments** | Using `nice()` values to influence the Completely Fair Scheduler (CFS) and measuring "accumulated iterations." | **Indirect Measurement:** We measure iterations of a loop rather than raw clock cycles, which can be influenced by other background tasks on the VM. | It provides a clear, human-readable metric of "work done" that demonstrates the visible impact of priority weights in a way raw nanosecond data cannot. |

---

## 6. Scheduler Experiment Results

The following experiments were conducted to observe how the Linux **Completely Fair Scheduler (CFS)** handles containers with different priorities and different workload types.

### Experiment 1: Priority Weighting (High vs. Low Nice)
We ran two identical `cpu_hog` workloads for 10 seconds. Container **C1** was given maximum priority (`--nice -20`) and **C2** was given minimum priority (`--nice 19`).

| Container | Nice Value | Host CPU % (Observed in `top`) | Total Iterations (Logs) |
|-----------|------------|--------------------------------|-------------------------|
| **C1** | -20        | ~99.5%                         | 14,209,331              |
| **C2** | 19         | ~0.5%                          | 210,442                 |

**Analysis:**
The results demonstrate the **Fairness** principle of CFS. CFS assigns a "weight" to each process based on its nice value. Because C1 had a much higher weight, its "virtual runtime" increased much more slowly than C2's. Consequently, the scheduler allowed C1 to occupy the CPU almost exclusively, only giving C2 enough time to prevent total starvation.

### Experiment 2: CPU-Bound vs. I/O-Bound
We ran one `cpu_hog` (constant math) alongside one `io_pulse` (sleeps for 200ms between small writes).

* **Observation:** The `cpu_hog` consistently showed 99% CPU usage. However, the `io_pulse` logs showed **zero latency spikes**. Every 200ms pulse was recorded exactly on time.
* **Explanation:** This demonstrates CFS's **Responsiveness**. Because the I/O-bound process spends most of its time sleeping, its "virtual runtime" stays very low. As soon as it wakes up, the scheduler sees it is the most "unfairly treated" process and immediately preempts the `cpu_hog` to let the I/O task run.

---
