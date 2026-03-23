# HGW-Doctor — Home Gateway Doctor

## Project overview
Autonomous health monitoring and self-recovery plugin for residential gateways
running PrplOS. Built as an Ambiorix plugin (.so) loaded by amxrt.

## Architecture
This project follows the same structure as amx-processmonitor (reference repo).
It is NOT a standalone daemon — it compiles to a shared library loaded by amxrt.

## Key conventions (from reference: amx-processmonitor)
- Entry point: `_hgwdoctor_main(int reason, amxd_dm_t* dm, amxo_parser_t* parser)`
  - reason 0 = START, reason 1 = STOP
- All data model writes use `amxd_trans_t` transactions — never direct assignment
- Logging uses `SAH_TRACEZ_INFO/WARNING/ERROR(ME, "...")` macros
- NULL checks use `when_null_trace(ptr, label, LEVEL, "msg")`
- The singleton is `static hgwdoctor_t hgwdoctor` in hgwdoctor_main.c
- Timer callbacks signature: `void cb(amxp_timer_t* timer, void* priv)`

## Ambiorix libraries available
- libamxc  — data containers (amxc_var_t, amxc_llist_t, amxc_htable_t)
- libamxp  — event loop, signals, timers (amxp_timer_*)
- libamxd  — data model (amxd_dm_t, amxd_object_t, amxd_trans_t)
- libamxo  — ODL parser
- libamxb  — bus abstraction (ubus/USP)

## Project structure
src/
  hgwdoctor_main.c   ← plugin entry point, singleton, timer loop
  hgwd_metrics.c     ← /proc reading: CPU, memory, process liveness
  hgwd_events.c      ← anomaly event emission via amxp_sigmngr
  hgwd_recovery.c    ← recovery actions: RESTART / KILL_TOP / DROP_CACHES / REBOOT
  hgwd_dm_methods.c  ← DM action callbacks bound in ODL

include_priv/
  hgwdoctor.h        ← shared types, struct definitions, function declarations

odl/
  hgw-doctor_definition.odl   ← data model schema (GatewayHealth object)
  hgw-doctor.odl              ← loader ODL for amxrt

## Data model (ODL)
Root object: GatewayHealth
  - Health (read-only string): Good / Warning / Critical / Unknown
  - CpuUsage, MemUsage (read-only uint32): live metrics from /proc
  - CpuThreshold, MemThreshold (persistent uint32): configurable limits
  - CpuRecoveryAction, MemRecoveryAction: NO_ACTION / KILL_TOP|DROP_CACHES / REBOOT
  - WatchedProcess[] instances: Name, PidFile, RecoveryAction, Health

## Build
make DEBUG=1              # native debug build
make CROSS_COMPILE=arm-openwrt-linux-  # cross-compile for Pi
amxrt odl/hgw-doctor.odl # run the plugin

## Target platform
Raspberry Pi 4 running PrplOS (OpenWrt-based)
Kernel interfaces: /proc/stat, /proc/meminfo, /proc/<pid>/