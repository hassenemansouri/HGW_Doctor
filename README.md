# HGW-Doctor

**Autonomous Health Monitoring Daemon for PrplOS Residential Gateways**

[![Platform](https://img.shields.io/badge/platform-PrplOS%20%7C%20OpenWrt-blue)]()
[![Language](https://img.shields.io/badge/language-C-lightgrey)]()
[![Framework](https://img.shields.io/badge/framework-Ambiorix-teal)]()
[![Protocol](https://img.shields.io/badge/protocol-TR--181%20%7C%20TR--069-orange)]()

---

## Overview

HGW-Doctor is a standalone C daemon integrated into PrplOS as an OpenWrt feed. It continuously monitors gateway health, detects sustained anomalies, dispatches autonomous recovery actions, collects diagnostic archives, and exposes a complete TR-181 data model accessible by the ISP's ACS via TR-069 — all without human intervention.

### Why HGW-Doctor?

ISPs managing thousands of deployed residential gateways face three core problems:

| Problem | Impact |
|---|---|
| No visibility into service health | ISP blind to memory leaks, CPU spikes, crashed services |
| Manual recovery only | Technician visit or customer reboot required |
| No diagnostic trace | Root cause analysis impossible after a fix |

HGW-Doctor solves all three automatically.

---

## Key Features

- **System monitoring** — CPU and memory usage via `/proc/stat` and `/proc/meminfo`
- **Per-process monitoring** — CPU%, memory%, liveness for each service in ProcessList
- **Sustained anomaly detection** — circular buffer approach prevents false positives from short spikes
- **5 anomaly types** — SystemCPU, SystemMem, Process (dead), ProcessCPU, ProcessMem
- **Autonomous recovery** — ProcessRestart, CacheClear, deferred Reboot
- **On-demand remote actions** — ISP writes `ActionType` via TR-069 to trigger immediate action
- **Targeted restart** — ISP sets `OnDemandTarget` to restart a specific service
- **Diagnostic collection** — automatic tar.gz archive (ps, dmesg, meminfo, event.txt)
- **HTTPS upload** — archives posted to ISP server via libcurl with retry logic
- **TR-181 data model** — full parameter tree under `Device.X_TELNET_HGWDoctor.*`
- **Live configuration** — all thresholds changeable at runtime via TR-069, no restart needed
- **PrplOS native** — OpenWrt feed, procd init, ipk package, ubus registration

---

## Architecture

```
ISP ACS (TR-069)
      │
      ▼
Device.X_TELNET_HGWDoctor   ← tr181-device proxy (mapping file)
      │
      ▼
HGWDoctor (ubus)             ← owned by hgw-doctor daemon
      │
      ├── monitor.c          reads /proc every PollInterval seconds
      ├── analyzer.c         sustained threshold detection (circular buffer)
      ├── recovery.c         ProcessRestart / CacheClear / Reboot
      ├── diag_collector.c   tar.gz snapshot per anomaly
      ├── uploader.c         HTTPS POST to ISP server (background thread)
      └── datamodel.c        amxd transactions — TR-181 parameter updates
```

---

## TR-181 Data Model

All parameters exposed under `HGWDoctor.*` and proxied automatically to `Device.X_TELNET_HGWDoctor.*` by tr181-device.

### Configuration (writable by ISP via TR-069)

| Parameter | Type | Default | Description |
|---|---|---|---|
| `Enable` | bool | true | Enable/disable all monitoring |
| `CPUThreshold` | uint32 | 90 | System CPU anomaly threshold (%) |
| `MemThreshold` | uint32 | 90 | System memory anomaly threshold (%) |
| `ThresholdDuration` | uint32 | 60 | Seconds metric must exceed threshold |
| `PollInterval` | uint32 | 5 | Metric collection interval (seconds) |
| `ActionType` | string | "None" | On-demand trigger: CacheClear / ProcessRestart / Reboot |
| `OnDemandTarget` | string | "" | Target process for on-demand ProcessRestart |
| `ProcessList` | string | "" | Comma-separated list of monitored services |
| `RebootDelaySec` | uint32 | 10 | Deferred reboot countdown (seconds) |
| `UploadURL` | string | "" | HTTPS endpoint for diagnostic uploads |

### Status (read-only, updated by daemon)

| Parameter | Description |
|---|---|
| `Status` | Enabled / Disabled / RebootPending / SafeMode |
| `AnomalyCount` | Total anomaly events since last reset |
| `LastActionType` | Last executed action |
| `LastActionStatus` | Success / Failure / None |
| `LastActionTime` | ISO 8601 timestamp of last action |
| `RebootCount` | Number of daemon-triggered reboots |
| `LastRebootTime` | ISO 8601 timestamp of last reboot |

### Live Statistics (`Stats.*`)

| Parameter | Description |
|---|---|
| `Stats.CurrentCPUUsage` | Current system CPU % |
| `Stats.CurrentMemUsage` | Current system memory % |
| `Stats.CurrentMemFreeKB` | Free memory in KB |
| `Stats.UptimeSeconds` | Daemon uptime |
| `Stats.TotalRecoveryActions` | Total recovery actions taken |

### AnomalyLog (multi-instance, read-only)

| Parameter | Description |
|---|---|
| `AnomalyLog.{i}.AnomalyType` | Process / ProcessCPU / ProcessMem / SystemCPU / SystemMem / OnDemand |
| `AnomalyLog.{i}.ProcessName` | Affected service name |
| `AnomalyLog.{i}.MetricValue` | Metric value at anomaly time (%) |
| `AnomalyLog.{i}.ActionTaken` | Recovery action executed |
| `AnomalyLog.{i}.ActionResult` | Success / Failure |
| `AnomalyLog.{i}.Timestamp` | ISO 8601 timestamp |

### MonitoredProcess[] (multi-instance)

| Parameter | Description |
|---|---|
| `MonitoredProcess.{i}.Name` | Service name (key) |
| `MonitoredProcess.{i}.Status` | Running / Dead / Unknown |
| `MonitoredProcess.{i}.PID` | Current process ID |
| `MonitoredProcess.{i}.CurrentCPU` | Live CPU usage (%) |
| `MonitoredProcess.{i}.CurrentMem` | Live memory usage (%) |
| `MonitoredProcess.{i}.CPUThreshold` | Per-service CPU threshold |
| `MonitoredProcess.{i}.MemThreshold` | Per-service memory threshold |
| `MonitoredProcess.{i}.ActionType` | Per-service recovery action |
| `MonitoredProcess.{i}.RestartCount` | Number of restarts |
| `MonitoredProcess.{i}.LastRestartTime` | ISO 8601 last restart time |

### Monitoring Profiles (`Profiles.*`)

Four built-in profiles switchable at runtime:

| Profile | CPUThreshold | MemThreshold | Duration | Action |
|---|---|---|---|---|
| default | 90% | 90% | 60s | ProcessRestart |
| aggressive | 70% | 75% | 30s | ProcessRestart |
| conservative | 95% | 95% | 120s | ProcessRestart |
| reboot-recovery | 98% | 98% | 180s | Reboot |

---

## On-Demand Remote Actions

The ISP can trigger immediate actions from the ACS via TR-069:

```sh
# Trigger cache clear immediately
ubus-cli "HGWDoctor.ActionType = 'CacheClear'"

# Restart a specific service immediately
ubus-cli "HGWDoctor.OnDemandTarget = 'cwmp_plugin'"
sleep 1
ubus-cli "HGWDoctor.ActionType = 'ProcessRestart'"

# Restart ALL monitored services
ubus-cli "HGWDoctor.ActionType = 'ProcessRestart'"

# Deferred reboot (10 second countdown)
ubus-cli "HGWDoctor.ActionType = 'Reboot'"
```

All on-demand actions are logged in `AnomalyLog` with `AnomalyType="OnDemand"` and `ActionType` resets to `"None"` automatically after execution.

---

## Repository Structure

```
HGW-Doctor/
├── src/
│   ├── main.c              Entry point, main event loop, signal handling
│   ├── monitor.c           /proc metric collection (system + per-process)
│   ├── analyzer.c          Sustained anomaly detection (circular buffers)
│   ├── recovery.c          Recovery action dispatch
│   ├── diag_collector.c    Diagnostic archive creation
│   ├── uploader.c          HTTPS upload via libcurl
│   ├── datamodel.c         TR-181 parameter management (Ambiorix)
│   └── config.c            INI configuration file parser
├── include/
│   ├── types.h             Shared types and constants
│   ├── monitor.h
│   ├── analyzer.h
│   ├── recovery.h
│   ├── datamodel.h
│   ├── diag_collector.h
│   ├── uploader.h
│   └── config.h
├── odl/
│   ├── hgw_doctor.odl          Main TR-181 data model definition
│   ├── hgw_doctor_defaults.odl Built-in profiles and default values
│   └── hgw_doctor_caps.odl     Bus registration and runtime config
├── actions/
│   ├── restart_process.sh   Service restart via procd init script
│   ├── clear_cache.sh       Kernel cache clear (/proc/sys/vm/drop_caches)
│   └── reboot_system.sh     System reboot
├── conf/
│   └── hgw_doctor.conf      INI configuration file
├── hgw-doctor/
│   ├── Makefile             OpenWrt package Makefile
│   └── files/
│       ├── hgw-doctor.init              procd init script
│       └── 01_device-hgw_doctor_mapping.odl  TR-181 proxy mapping
└── tests/
    └── test-service/        Companion Ambiorix daemon for validation
```

---

## Build & Integration

### Prerequisites

- Docker container with PrplOS build environment
- GitHub repository configured as OpenWrt feed in `feeds.conf`

### Build

```sh
# Update feed
./scripts/feeds update hgw_doctor

# Compile package only
make package/hgw-doctor/compile V=s

# Full image build
make -j$(nproc)
```

### Flash

```sh
# On development laptop
gunzip -c prplos-bcm27xx-bcm2711-rpi-4-ext4-factory.img.gz \
  | sudo dd of=/dev/sdb bs=4M status=progress conv=fsync oflag=sync
sudo sync
```

---

## Test Service

A companion Ambiorix daemon (`test-service`) is provided for validation. It registers `TestService` on ubus with three simulation modes:

```sh
# Simulate service crash
ubus call TestService Crash '{}'

# Simulate CPU stress (busy loop)
ubus call TestService SetMode '{"mode":"CPUStress"}'

# Simulate memory stress (~600 MB allocation)
ubus call TestService SetMode '{"mode":"MemStress"}'

# Return to idle
ubus call TestService SetMode '{"mode":"Idle"}'
```

---

## Testing

### Quick validation after flash

```sh
# Check daemon running
pidof hgw-doctor && echo "RUNNING"
ubus call HGWDoctor _get | grep -E "Status|AnomalyCount|ActionType"

# Test on-demand CacheClear
ubus-cli "HGWDoctor.ActionType = 'CacheClear'"
sleep 5
ubus call HGWDoctor _get | grep -E "LastAction|AnomalyCount"

# Test process crash recovery
ubus-cli "HGWDoctor.CPUThreshold = 25"
ubus-cli "HGWDoctor.ThresholdDuration = 5"
ubus-cli "HGWDoctor.ProcessList = 'test-service'"
ubus call TestService Crash '{}'
sleep 15
ubus-cli "HGWDoctor.AnomalyLog.?" | grep -E "AnomalyType|ActionTaken|ProcessName|ActionResult"
```

### TR-181 mapping check

```sh
echo "--- HGWDoctor ---"
ubus-cli "HGWDoctor.?" | grep -E "AnomalyCount|LastActionType|Status"
echo "--- Device.X_TELNET_HGWDoctor ---"
ubus-cli "Device.X_TELNET_HGWDoctor.?" | grep -E "AnomalyCount|LastActionType|Status"
```

---

## Configuration

Default configuration at `/etc/hgw_doctor/hgw_doctor.conf`:

```ini
CPUThreshold=90
MemThreshold=90
ThresholdDuration=60
PollInterval=5
ProcessList=cwmp_plugin,tr181-device,dhcpv4-manager,tr181-dns,test-service
ScriptsDir=/usr/lib/hgw_doctor/actions
DiagOutputDir=/tmp/hgw_diag
UploadURL=https://diagnostics.telnet.tn/upload
```

All parameters are also configurable live via TR-069 without daemon restart.

---

## Dependencies

| Library | Purpose |
|---|---|
| libamxd | Ambiorix data model (TR-181 parameter tree) |
| libamxo | Ambiorix ODL parser |
| libamxb | Ambiorix bus interface (ubus) |
| libamxc | Ambiorix variant type system |
| libamxp | Ambiorix signal/slot event system |
| libcurl | HTTPS diagnostic upload |

---

## Author

**Hassene Mansouri**  
Systems Engineering Intern — Telnet, Tunis  
Esprit — École Supérieure Privée d'Ingénierie et de Technologies  
2025–2026

**Repository:** https://github.com/hassenemansouri/HGW_Doctor  
**Branch:** HGW_Doctor_0  
**test-service:** https://github.com/hassenemansouri/test-service
