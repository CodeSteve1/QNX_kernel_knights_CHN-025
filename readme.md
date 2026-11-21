# QNX Mixed-Criticality Automotive Cockpit (Raspberry Pi 5)

> **Crash the media player all you want—Core 3 doesn't care.**  
> A fault-tolerant, mixed-criticality automotive instrument cluster and In-Vehicle Infotainment (IVI) system built on **QNX Neutrino RTOS (SDP 8.0)** for the **Raspberry Pi 5**.

[![OS](https://img.shields.io/badge/RTOS-QNX%20Neutrino%20SDP%208.0-0078D7.svg)](#)
[![Hardware](https://img.shields.io/badge/Target-Raspberry%20Pi%205%20(aarch64le)-C51A4A.svg)](#)
[![Language](https://img.shields.io/badge/Language-C%20%2F%20OpenGL%20ES%203-555555.svg)](#)
[![Architecture](https://img.shields.io/badge/Architecture-Microkernel%20%7C%20BMP%20Isolation-2EA44F.svg)](#)

---

## Table of Contents

1. [Project Overview](#1-project-overview)
2. [Why QNX Neutrino RTOS?](#2-why-qnx-neutrino-rtos)
3. [System Architecture](#3-system-architecture)
4. [Process & Thread Breakdown](#4-process--thread-breakdown)
5. [Core Isolation & Priority Scheduling](#5-core-isolation--priority-scheduling)
6. [QNX Native IPC & Latency Measurement](#6-qnx-native-ipc--latency-measurement)
7. [Self-Healing Supervision (Guardian–Worker Pattern)](#7-self-healing-supervision-guardianworker-pattern)
8. [Hardware Architecture & RP1 Wiring](#8-hardware-architecture--rp1-wiring)
9. [Build & Deployment Guide](#9-build--deployment-guide)
10. [Live Verification & Profiling Commands](#10-live-verification--profiling-commands)
11. [Fault-Injection & Recovery Demo Playbook](#11-fault-injection--recovery-demo-playbook)
12. [Troubleshooting](#12-troubleshooting)

---

## 1. Project Overview

Modern Software-Defined Vehicles (SDVs) consolidate **safety-critical real-time workloads** (digital instrument clusters, hazard alerts, audio warnings) and **heavy infotainment workloads** (hardware-accelerated video playback, OpenGL UIs) onto a single System-on-Chip (SoC). Running both domains together without strict isolation creates a severe safety risk: a GPU driver fault, memory leak, or `SIGSEGV` in the media player can stall or crash the speedometer.

This project demonstrates a complete **mixed-criticality cockpit** on a single **Raspberry Pi 5** running **QNX Neutrino RTOS**, engineered around four pillars:

* **Spatial Isolation (Bound Multiprocessing):** Hard CPU core pinning via `ThreadCtl(_NTO_TCTL_RUNMASK)` separates the real-time cluster (**Core 3**) from the OpenGL ES 3 infotainment stack (**Cores 0–2**).
* **Deterministic Real-Time Telemetry:** Direct user-space RP1 GPIO polling, UART speed ingestion, zero-copy QNX message passing (`MsgSend`/`MsgReceive`), and hardware `ClockCycles()` end-to-end latency profiling.
* **Interactive Fault-Injection IVI:** A 1920×1080 QNX Screen + OpenGL ES 3 multi-instance video player featuring live per-core CPU telemetry, optional CPU/GPU stress burning, and a dedicated **Crash (`SIGSEGV`)** button for live demos.
* **Zero-Blocking Self-Healing:** A two-tier **Guardian + Supervisor Worker** watchdog built on non-blocking **QNX Pulses** (`MsgSendPulse`) that detects crashes and hangs across all processes—and can even restart itself while adopting healthy children with zero downtime.

---

## 2. Why QNX Neutrino RTOS?

| Capability | QNX Neutrino RTOS | Standard Linux | FreeRTOS / Bare-Metal |
| :--- | :--- | :--- | :--- |
| **Kernel Architecture** | **Microkernel** — drivers, filesystems, and apps run in isolated user-space processes | **Monolithic kernel** — drivers share kernel address space | Flat single address space (no MMU process isolation) |
| **Fault Containment** | A `SIGSEGV` or driver crash in IVI **cannot** crash the kernel or Core 3 cluster | A faulty GPU/video driver can trigger a **kernel panic**, blacking out the cluster | A null pointer or stack overflow halts the entire MCU |
| **Scheduling Determinism** | **Hard real-time** priority-based preemptive scheduler (`SCHED_FIFO` / `SCHED_RR`) | Fairness-based (CFS); background I/O or page faults cause latency jitter | Hard real-time, deterministic |
| **Mixed-Criticality** | Runs OpenGL ES 3 video (Cores 0–2) and real-time SPI/GPIO (Core 3) on **one SoC** | Struggles to guarantee hard deadlines under heavy GPU/video load | Cannot run full OpenGL ES 3 / HDMI video stacks or POSIX multi-process apps |
| **IPC & Watchdogs** | Native **non-blocking pulses** (`MsgSendPulse`) and priority-inheriting `MsgSend` | Sockets/pipes/D-Bus add copy overhead and lack native priority inheritance | Basic queues; no protected process boundary for self-healing restarts |

---

## 3. System Architecture

```text
+-----------------------------------------------------------------------------+
|                        Raspberry Pi 5 (QNX Neutrino)                        |
|                                                                             |
|  +------------------------------+      +---------------------------------+  |
|  |     CORES 0 - 2 (IVI)        |      |       CORE 3 (REAL-TIME)        |  |
|  |                              |      |                                 |  |
|  |  +------------------------+  |      |  +---------------------------+  |  |
|  |  |  media_player_test_v4  |  |      |  |       sensor_msg_v1       |  |  |
|  |  |  • QNX Screen / HDMI   |  |      |  | • RP1 GPIO (17, 27, 22)   |  |  |
|  |  |  • OpenGL ES 3 (1080p) |  |      |  | • UART (/dev/ser1) 115200 |  |  |
|  |  |  • Multi-Video + GUI   |  |      |  | • ClockCycles() Timestamp |  |  |
|  |  |  • /proc/1/as CPU Mon  |  |      |  +-------------+-------------+  |  |
|  |  |  • SIGSEGV Crash Btn   |  |      |                | QNX MsgSend    |  |
|  |  +-----------+------------+  |      |                | (Synchronous)  |  |
|  |              |               |      |                v                |  |
|  +--------------+---------------+      |  +---------------------------+  |  |
|                 |                      |  |        lcd_proc_v1        |  |  |
|                 | Pulse HB             |  | • IPC: lcd_dashboard_srv  |  |  |
|                 | (7s timeout)         |  | • ILI9341 SPI (12 MHz)    |  |  |
|                 |                      |  | • E2E Latency Display     |  |  |
|                 |                      |  | • Buzzer Thread (GPIO 18) |  |  |
|                 |                      |  +-------------+-------------+  |  |
|                 |                      |                | Pulse HB       |  |
|                 |                      |                | (4s timeout)   |  |
|                 +-------------------+-------------------+                |  |
|                                     |                                    |  |
|                                     v                                    |  |
|                  +-------------------------------------+                 |  |
|                  |            supervisor_v1            |                 |  |
|                  |   Channel: qnx_supervisor           |                 |  |
|                  |   Guardian <=== Pulse HB ===> Worker|                 |  |
|                  +-------------------------------------+                 |  |
+-----------------------------------------------------------------------------+
```

---

## 4. Process & Thread Breakdown

| Process | Threads | Target Core(s) | Primary Responsibility | Key QNX / Hardware APIs |
| :--- | :---: | :---: | :--- | :--- |
| **`sensor_msg_v1`** | 1 | Core 3 | Polls ESP32 hazard GPIOs (5 ms loop) and parses UART speed frames (`S<0-240>\n`). Captures `ClockCycles()` timestamp and sends telemetry to `lcd_proc_v1`. | `ThreadCtl(_NTO_TCTL_IO)`, `mmap_device_memory`, `termios`, `MsgSend`, `MsgSendPulse` |
| **`lcd_proc_v1`** | 2 | Core 3 | **TID 1:** IPC server (`lcd_dashboard_srv`) that replies immediately to unblock the sensor, computes E2E latency, and renders the ILI9341 SPI cluster.<br>**TID 2:** Dedicated passive buzzer tone-generator thread. | `name_attach`, `MsgReceive`, `MsgReply`, `TimerTimeout`, `io-spi`, `pthread_create` |
| **`media_player_test_v4`** | 3+ | Cores 0–2 | 1920×1080 QNX Screen + OpenGL ES 3 multi-instance video player with pagination, off-page GPU stress rendering, live `/proc/1/as` CPU sampling, and a `SIGSEGV` crash trigger. | `screen`, `EGL/GLES3`, `devctl(DCMD_PROC_TIDSTATUS)`, `MsgSendPulse`, `pthread_setschedparam` |
| **`supervisor_v1`** | 2 procs (1 thread each) | Core 3 | **Guardian + Supervisor Worker** pair. Spawns and monitors all 3 processes via non-blocking QNX pulses (`qnx_supervisor`). Automatically restarts any crashed or hung process—including itself. | `ChannelCreate`, `MsgReceivePulse`, `TimerTimeout`, `fork`/`execv`, `waitpid`, `kill` |

---

## 5. Core Isolation & Priority Scheduling

### Spatial Isolation (Bound Multiprocessing)
Each process restricts its threads to specific CPU cores using QNX's `ThreadCtl(_NTO_TCTL_RUNMASK, ...)`:
* **Core 3 (`runmask = 0x08`, `1U << 3`):** Dedicated exclusively to safety-critical cluster tasks (`sensor_msg_v1`, `lcd_proc_v1`, `supervisor_v1`).
* **Cores 0–2 (`runmask = 0x01` / `0x07`):** Dedicated to `media_player_test_v4` render, decode, and optional stress-burner threads.

When `ThreadCtl(_NTO_TCTL_RUNMASK, ...)` is called, the QNX kernel writes the bitmask directly into the thread's **Thread Control Block (TCB)** inside `procnto`. The scheduler filters every dispatch against this mask, guaranteeing that IVI threads can never contend for Core 3.

### Priority-Based Preemptive Scheduling (`SCHED_FIFO`)
QNX Neutrino is a priority-based preemptive RTOS (priorities `0`–`255`, default `10`). To prevent **watchdog starvation** (where a hung child in a 100% CPU loop blocks the supervisor) and ensure strict determinism on Core 3, the recommended `SCHED_FIFO` priority hierarchy is:

| Process / Thread | Priority (`SCHED_FIFO`) | Rationale |
| :--- | :---: | :--- |
| **`supervisor_v1`** | **28** | Highest on Core 3 so the watchdog can always preempt a hung child; stays blocked in `MsgReceivePulse()` 99.9% of the time. |
| **`sensor_msg_v1`** | **25** | Captures time-critical GPIO hazards and UART speed telemetry immediately. |
| **`lcd_proc_v1` (TID 1)** | **24** | Renders the SPI display immediately after receiving the sensor IPC message. |
| **`lcd_proc_v1` (Buzzer TID 2)** | **18** | Runs its tone loop only when higher-priority threads are blocked. |
| **`media_player_test_v4`** | **10 (default)** | Isolated on Cores 0–2; optional `STRESS_BURN` threads explicitly lower their priority by 1 via `pthread_setschedparam()` so rendering always preempts burners. |

---

## 6. QNX Native IPC & Latency Measurement

### Synchronous Telemetry (`sensor_msg_v1` → `lcd_proc_v1`)
1. `lcd_proc_v1` registers a global IPC endpoint at `/dev/name/local/lcd_dashboard_srv` using `name_attach()`.
2. `sensor_msg_v1` reads the ESP32 hazard pins and UART speed, records the exact hardware cycle count via `ClockCycles()`, and sends a structured payload via `MsgSend()`.
3. `lcd_proc_v1` receives the message via `MsgReceive()`, copies the payload, and **calls `MsgReply()` immediately** before starting the SPI draw—ensuring the sensor polling loop never stalls while the ILI9341 screen updates.
4. Once the SPI frame completes, `lcd_proc_v1` reads `ClockCycles()` again and calculates exact end-to-end latency in milliseconds/microseconds to display on the cluster.

### Asynchronous Heartbeats (All Processes → `supervisor_v1`)
* Every monitored process opens a connection to `qnx_supervisor` and periodically fires a fixed-size, non-blocking **QNX Pulse** via `MsgSendPulse()`.
* Because `MsgSendPulse()` never blocks the sender, a hung or crashed supervisor can **never** freeze `sensor_msg_v1` or `lcd_proc_v1`.

---

## 7. Self-Healing Supervision (Guardian–Worker Pattern)

To eliminate any single point of failure, `supervisor_v1` splits into two cooperating processes:

1. **Supervisor Worker:**
   * Spawns `lcd_proc_v1`, `sensor_msg_v1`, and `media_player_test_v4` via `fork()` + `execv()`.
   * Listens on `qnx_supervisor` using `MsgReceivePulse()` bounded by `TimerTimeout()`.
   * **Crash detection:** Detects immediate child exits via `waitpid(..., WNOHANG)` and respawns the process within ~500 ms.
   * **Hang detection:** If a child misses its heartbeat deadline (**4 seconds** for `lcd_proc_v1` / `sensor_msg_v1`, **7 seconds** for `media_player_test_v4`), the supervisor sends `SIGKILL` and spawns a fresh instance.
   * Sends its own periodic heartbeat pulse to the Guardian.

2. **Guardian:**
   * Monitors the Supervisor Worker via QNX pulses (**4-second** timeout).
   * If the Supervisor Worker crashes (`kill -9`) or hangs (`kill -STOP`), the Guardian kills the hung worker, spawns a new Supervisor Worker, and **adopts the 3 healthy running children** with zero interruption to the dashboard or video playback.

---

## 8. Hardware Architecture & RP1 Wiring

All low-level GPIO control on the Raspberry Pi 5's **RP1 southbridge** is performed directly from user-space using `ThreadCtl(_NTO_TCTL_IO)` and `mmap_device_memory()`.

| Peripheral | Signal | BCM GPIO | Interface / Configuration |
| :--- | :--- | :---: | :--- |
| **ESP32 Hazard Controller** | Hazard 1 (Warning) | `GPIO 17` | RP1 Input, Pull-Down, Schmitt Trigger |
| **ESP32 Hazard Controller** | Hazard 2 (Critical) | `GPIO 27` | RP1 Input, Pull-Down, Schmitt Trigger |
| **ESP32 Hazard Controller** | Hazard 3 (Alert) | `GPIO 22` | RP1 Input, Pull-Down, Schmitt Trigger |
| **ESP32 Speed Telemetry** | UART RX / TX | `/dev/ser1` | 115200 baud, 8N1, ASCII frames `S<0-240>\n` |
| **ILI9341 2.4" TFT LCD** | SPI Bus (MOSI/SCLK) | `/dev/io-spi/spi0/dev0` | `spi-dwc` driver, 12 MHz, 8-bit MSB |
| **ILI9341 2.4" TFT LCD** | DC / RST / CS | `GPIO 26` / `19` / `13` | Direct RP1 `RIO_OUT` register control |
| **Passive Buzzer** | Audio Alert Output | `GPIO 18` | Dedicated pthread tone generator via `ClockCycles()` |
| **IVI Display** | HDMI Output | HDMI 0 | 1920×1080 via QNX `screen` + `drm-rpi5` (OpenGL ES 3) |

---

## 9. Build & Deployment Guide

### Prerequisites
* **Host:** QNX Software Development Platform (SDP) 8.0 (`qcc`, `aarch64le` toolchain)
* **Target:** Raspberry Pi 5 running QNX Neutrino RTOS 8.0 with `screen`, `drm-rpi5`, `spi-dwc`, and UART (`/dev/ser1`) active.

### 1. Build Binaries (Host)
Ensure `supervisor_pulse.h` and `ipc_msg.h` are in your include path, then build all four projects for `aarch64le` in QNX Momentics or via `qcc`:

```bash
# Example command-line build pattern
qcc -Vgcc_ntoaarch64le -O2 -Wall -o supervisor_v1 supervisor_v1.c
```

### 2. Deploy to Raspberry Pi 5
```bash
scp -r ~/qnx_finals/* qnxuser@<PI_IP>:/data/home/qnxuser/bin/
```

### 3. Launch the Cockpit (Target)
Use `sudo -E` to preserve graphics environment variables (`GRAPHICS_ROOT`, `LD_LIBRARY_PATH`) while granting `ThreadCtl(_NTO_TCTL_IO)` privileges for RP1 GPIO access:

```sh
sudo -E /data/home/qnxuser/bin/supervisor_v1/build/aarch64le-debug/supervisor_v1
```

### 4. Stop Cleanly
* Press **`Ctrl + C`** in the supervisor terminal to gracefully shut down all managed processes, **or** force-stop all four processes:
  ```sh
  sudo slay -f supervisor_v1 lcd_proc_v1 sensor_msg_v1 media_player_test_v4
  ```

---

## 10. Live Verification & Profiling Commands

### Inspect All 4 Processes, Threads, Priorities, and CPU Cores
Use `%n` (full path) instead of `%N` so long names like `media_player_test_v4` are never truncated:

```sh
pidin -F "%a %b %n %p %l %J" | grep -E "pid|supervisor_v1|lcd_proc_v1|sensor_msg_v1|media_player_test_v4"
```

* `%a` = PID | `%b` = Thread ID (TID) | `%n` = Full Process Name | `%p` = Priority & Policy | `%l` = Last CPU Core (`0`–`3`) | `%J` = Thread State

### Live-Refreshing Process & Core Monitor (1-Second Interval)
```sh
while true; do clear; pidin -F "%a %b %n %p %l %J" | grep -E "pid|supervisor_v1|lcd_proc_v1|sensor_msg_v1|media_player_test_v4"; sleep 1; done
```

### Real-Time CPU Usage (`hogs`)
```sh
# Live update every 1 second, sorted by CPU %
hogs -n -s 1
```

---

## 11. Fault-Injection & Recovery Demo Playbook

| Demo Scenario | How to Trigger | Expected System Behavior | Core 3 Cluster Impact |
| :--- | :--- | :--- | :--- |
| **1. IVI Crash (`SIGSEGV`)** | Click the **Crash** button in the media player GUI | `media_player_test_v4` segfaults immediately; `supervisor_v1` detects exit via `waitpid` and respawns it in ~500 ms. | **Zero** — speedometer, hazards, and buzzer continue uninterrupted. |
| **2. Process Hang (`SIGSTOP`)** | `sudo kill -STOP <pid>` on `lcd_proc_v1`, `sensor_msg_v1`, or `media_player_test_v4` | Supervisor detects missed heartbeat pulses via `TimerTimeout` (4 s for cluster, 7 s for IVI), sends `SIGKILL`, and spawns a clean instance. | Auto-restored within 4 s. |
| **3. Supervisor Crash** | `sudo kill -9 <supervisor_worker_pid>` | Guardian detects worker exit, spawns a new Supervisor Worker, and **adopts** the 3 running children. | **Zero downtime** — dashboard and video never blink. |
| **4. Supervisor Hang** | `sudo kill -STOP <supervisor_worker_pid>` | Guardian detects missed supervisor heartbeat after 4 s, kills the hung worker, and restarts it seamlessly. | **Zero downtime** — non-blocking pulses prevent child freezes. |

---

## 12. Troubleshooting

* **`media_player_test_v4` fails to launch under `sudo`:** `sudo` strips `LD_LIBRARY_PATH` and `GRAPHICS_ROOT`, breaking QNX `screen`/EGL initialization. Always launch the supervisor with `sudo -E`.
* **Video file not found when launched by supervisor:** Use absolute paths (e.g., `/data/home/qnxuser/qnx_vid.mp4`) rather than relative paths (`./qnx_vid.mp4`), since the supervisor's working directory may differ.
* **`pidin` prints `couldn't fill_timers`:** Avoid the `%R` format flag without `sudo`; use `%l` (last CPU core) and `%i` (runmask) instead.
* **Processes showing `cpu = 0` instead of `3`:** Verify that the deployed binaries in `/data/home/qnxuser/bin/` are the latest builds containing `ThreadCtl(_NTO_TCTL_RUNMASK, (void *)(1U << 3))` in `main()`.