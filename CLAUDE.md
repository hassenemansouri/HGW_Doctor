# HGW-Doctor — Home Gateway Doctor

## What this project is
Autonomous health monitoring and self-recovery **standalone daemon** for
residential gateways running PrplOS (OpenWrt-based Linux on Raspberry Pi 4).

It monitors CPU usage, memory usage, and process liveness. When thresholds are
breached it executes recovery actions (process restart, cache drop, reboot),
collects diagnostics (tar.gz archive of /proc snapshots), and uploads them via
HTTPS. All state is exposed to the ACS via a TR-181 data model over the
Ambiorix bus.

---

## Architecture — two-binary design

```
build/bin/hgw-doctor        ← main daemon executable
build/lib/hgw_doctor.so     ← Ambiorix data model callbacks (.so loaded by ODL)
```

The daemon is a **standalone executable with a main()** — NOT an amxrt plugin.
The `.so` exists only to provide ODL callback symbols (`dm_*` functions) to
the Ambiorix runtime embedded inside the daemon.

---

## Threading model

```
main thread
  ├── monitor thread (pthread)   ← polls /proc every poll_interval_s
  ├── analyzer thread (pthread)  ← watches circular buffer, fires anomaly callbacks
  └── event loop (sleep(1))      ← SIGHUP reload, SIGUSR1 on-demand diag, uptime counter
```

---

## Source files and their status

| File | Status | Role |
|------|--------|------|
| `src/main.c` | **TODO** | Entry point, signal handlers, module wiring, event loop |
| `src/monitor.c` | DONE | /proc/stat + /proc/meminfo + process liveness, circular buffer |
| `src/analyzer.c` | DONE | Sustained threshold detection, anomaly_callback dispatch |
| `src/recovery.c` | DONE | process restart / cache clear / reboot via scripts or direct |
| `src/datamodel.c` | DONE | Ambiorix TR-181 write-backs, RPC handlers (dm_* functions) |
| `src/config.c` | DONE | INI-style config file load/reload |
| `src/logger.c` | DONE | syslog wrapper with LOG_ERROR/WARN/INFO/DEBUG macros |
| `src/diag_collector.c` | DONE | tar.gz archive: /proc/stat, /proc/meminfo, ps, dmesg |
| `src/uploader.c` | DONE | libcurl HTTPS POST with retry logic |

---

## Key data flow

```
monitor.c  →  MetricCircBuf  →  analyzer.c
                                     │ anomaly_callback (on_anomaly in main.c)
                                     ▼
                               recovery.c (recovery_dispatch)
                                     │ recovery_callback (on_recovery_done in main.c)
                                     ▼
                               diag_collector.c (diag_collect)
                                     │ diag_done_callback (on_diag_done in main.c)
                                     ▼
                               uploader.c (uploader_send)
                                     │ upload_done_callback (on_upload_done in main.c)
                                     ▼
                               datamodel.c (datamodel_record_action etc.)
```

---

## Core types (include/types.h)

```c
MetricSnapshot   — cpu_pct, mem_used_pct, mem_free_kb, proc_alive, proc_pid, ts
MetricCircBuf    — slots[120], head (circular buffer, 10min at 5s interval)
AnomalyEvent     — type (CPU/MEMORY/PROCESS), metric_value, duration_s, detected_at
AnomalyType      — ANOMALY_NONE=0, ANOMALY_CPU=1, ANOMALY_MEMORY=2, ANOMALY_PROCESS=3
ActionType       — ACTION_NONE=0, ACTION_PROCESS_RESTART=1, ACTION_CACHE_CLEAR=2, ACTION_REBOOT=3
RecoveryResult   — action, result, executed_at, process_name, exit_code
UploadStatus     — NONE=0, PENDING=1, SUCCESS=2, FAILED=3
```

---

## Configuration (conf/hgw_doctor.conf)

```ini
CPUThreshold=95          # % CPU before anomaly (default 85)
MemThreshold=95          # % memory before anomaly (default 90)
ThresholdDuration=60     # seconds threshold must be sustained
PollInterval=5           # seconds between /proc samples
ProcessName=hgw-doctor   # process name to monitor for liveness
ScriptsDir=/usr/lib/hgw_doctor/actions
DiagOutputDir=/tmp/hgw_diag
ODLPath=/etc/amx/hgw_doctor/hgw_doctor.odl
```

---

## Data model (ODL) — Device.X_TELNET_HGWDoctor

Root object with:
- `Enable`, `Status`, `Profile`
- `CPUThreshold`, `MemThreshold`, `ThresholdDuration`, `PollInterval`
- `ActionType`, `ProcessName`
- `LastActionType`, `LastActionTime`, `LastActionStatus`, `AnomalyCount` (read-only)
- `OnDemandTrigger`, `DiagArchivePath`, `UploadURL`, `UploadStatus`
- `Stats.CurrentCPUUsage`, `Stats.CurrentMemUsage`, `Stats.CurrentMemFreeKB`
- `Stats.TotalRecoveryActions`, `Stats.TotalDiagUploads`, `Stats.UptimeSeconds`
- `Profiles[]` — multi-instance per-profile config
- `AnomalyLog[]` — multi-instance event log (capped at 50)
- RPCs: `TriggerDiagnostics()`, `ResetCounters()`, `SetProfile(ProfileName)`

TR-181 parameter paths are defined in `include/tr181_params.h`.

---

## What main.c must implement

### Globals needed
```c
static volatile sig_atomic_t g_running    = 1;
static volatile sig_atomic_t g_reload_cfg = 0;
static volatile sig_atomic_t g_diag_req   = 0;
static amxd_dm_t     g_dm;
static amxo_parser_t g_parser;
static MetricCircBuf g_metric_buf;
```

### Signal handlers
```c
void install_signals(void);
// SIGTERM/SIGINT → g_running = 0
// SIGHUP        → g_reload_cfg = 1
// SIGUSR1       → g_diag_req = 1
```

### Callbacks — wiring all modules together
```c
// on_anomaly       → calls recovery_dispatch(event)
// on_recovery_done → calls datamodel_record_action(result),
//                    datamodel_increment_anomaly_count(),
//                    datamodel_append_anomaly_log(),
//                    diag_collect(event)
// on_diag_done     → calls uploader_send(archive_path),
//                    dm update for DiagArchivePath
// on_upload_done   → calls datamodel_record_upload(status, path)
```

---

## Build

```sh
make                          # native build
make CROSS_COMPILE=aarch64-openwrt-linux-musl- \
     STAGING_DIR=/prplos-build/staging_dir/target-aarch64_cortex-a72_musl
make install
```

## Run

```sh
build/bin/hgw-doctor                         # default config
build/bin/hgw-doctor /path/to/custom.conf    # explicit config path
logread | grep hgw-doctor                    # check syslog on target
kill -SIGHUP  $(pidof hgw-doctor)            # reload config
kill -SIGUSR1 $(pidof hgw-doctor)            # trigger on-demand diag
```

## Dependencies
- `libamxd`, `libamxo`, `libamxc`, `libamxp`, `libamxs` — Ambiorix data model
- `libcurl` — HTTPS diagnostic upload
- `libpthread` — monitor and analyzer threads
- `libm` — math

## Target platform
- Hardware: Raspberry Pi 4
- OS: PrplOS / OpenWrt-based Linux
- Toolchain: GCC, Make, GDB, Valgrind