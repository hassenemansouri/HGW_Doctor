#!/bin/sh
# =============================================================================
# HGW-Doctor integration test
#
# Tests:
#   1. Process crash detection  — kill test-service, expect ANOMALY_PROCESS
#   2. CPU anomaly detection    — shell busy-loop on all cores
#   3. Memory anomaly detection — dd to tmpfs to consume RAM
#   4. On-demand diag trigger   — SIGUSR1 → archive created
#   5. Per-process CPU anomaly  — ubus TestService CPUStress  (skipped if no ubus)
#   6. Per-process Mem anomaly  — ubus TestService MemStress  (skipped if no ubus)
#
# Requirements:
#   - hgw-doctor binary at /tmp/hgw_test/bin/hgw-doctor
#   - config    at /tmp/hgw_test/conf/hgw_doctor.conf
# =============================================================================

set -eu

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'

pass() { printf "${GREEN}[PASS]${NC} %s\n" "$*"; }
fail() { printf "${RED}[FAIL]${NC} %s\n" "$*"; FAILURES=$((FAILURES+1)); }
info() { printf "${YELLOW}[INFO]${NC} %s\n" "$*"; }

FAILURES=0
HGW_BIN=/tmp/hgw_test/bin/hgw-doctor
SVC_BIN=/tmp/hgw_test/bin/test-service
HGW_CONF=/tmp/hgw_test/conf/hgw_doctor.conf
DIAG_DIR=/tmp/hgw_diag
HGW_LOG=/tmp/hgw_test/hgw-doctor.log
SVC_LOG=/tmp/hgw_test/test-service.log
HGW_PID=""
SVC_PID=""
CPU_STRESS_PIDS=""
MEM_STRESS_FILE=/tmp/hgw_memstress

cleanup() {
    info "Cleaning up..."
    ubus call TestService SetMode '{"mode":"Idle"}' 2>/dev/null || true
    [ -n "$HGW_PID" ] && kill "$HGW_PID" 2>/dev/null && wait "$HGW_PID" 2>/dev/null || true
    [ -n "$SVC_PID" ] && kill "$SVC_PID" 2>/dev/null && wait "$SVC_PID" 2>/dev/null || true
    [ -n "$CPU_STRESS_PIDS" ] && kill $CPU_STRESS_PIDS 2>/dev/null || true
    rm -f "$MEM_STRESS_FILE"
}
trap cleanup EXIT

wait_for_log() {
    local pattern="$1" timeout="${2:-30}" log="$3"
    local elapsed=0
    while ! grep -q "$pattern" "$log" 2>/dev/null; do
        sleep 1; elapsed=$((elapsed+1))
        [ $elapsed -ge $timeout ] && return 1
    done
    return 0
}

start_hgw() {
    info "Starting hgw-doctor..."
    HGW_LOG_STDERR=1 \
        "$HGW_BIN" "$HGW_CONF" > "$HGW_LOG" 2>&1 &
    HGW_PID=$!
    sleep 2
    if ! kill -0 "$HGW_PID" 2>/dev/null; then
        fail "hgw-doctor failed to start — check $HGW_LOG"
        cat "$HGW_LOG"
        exit 1
    fi
    info "hgw-doctor running (pid=$HGW_PID)"
}

start_svc() {
    info "Starting test-service..."
    "$SVC_BIN" > "$SVC_LOG" 2>&1 &
    SVC_PID=$!
    sleep 1
    if ! kill -0 "$SVC_PID" 2>/dev/null; then
        info "test-service binary unavailable — using shell stub (named process)"
        # rename the process to 'test-service' so hgw-doctor can watch it by name
        sh -c 'exec -a test-service sleep 3600' &
        SVC_PID=$!
    fi
    info "test-service running (pid=$SVC_PID)"
}

cpu_stress_start() {
    NCPU=$(grep -c "^processor" /proc/cpuinfo 2>/dev/null || echo 1)
    info "Launching CPU stress on $NCPU core(s)..."
    CPU_STRESS_PIDS=""
    i=0
    while [ $i -lt "$NCPU" ]; do
        (while :; do :; done) &
        CPU_STRESS_PIDS="$CPU_STRESS_PIDS $!"
        i=$((i+1))
    done
    info "CPU stress pids:$CPU_STRESS_PIDS"
}

cpu_stress_stop() {
    [ -n "$CPU_STRESS_PIDS" ] && kill $CPU_STRESS_PIDS 2>/dev/null || true
    [ -n "$CPU_STRESS_PIDS" ] && wait $CPU_STRESS_PIDS 2>/dev/null || true
    CPU_STRESS_PIDS=""
    info "CPU stress stopped"
}

mem_stress_start() {
    TOTAL_MEM_KB=$(grep MemTotal /proc/meminfo | awk '{print $2}')
    TOTAL_MEM_MB=$((TOTAL_MEM_KB / 1024))
    ALLOC_MB=$(( (TOTAL_MEM_MB * 75) / 100 ))
    info "System RAM: ${TOTAL_MEM_MB} MB — allocating ${ALLOC_MB} MB via tmpfs..."
    # Write to /tmp (tmpfs) — keeps pages in RAM until file is deleted
    dd if=/dev/zero of="$MEM_STRESS_FILE" bs=1M count=$ALLOC_MB 2>/dev/null
    info "Memory stress file written (${ALLOC_MB} MB in RAM)"
}

mem_stress_stop() {
    rm -f "$MEM_STRESS_FILE"
    info "Memory stress released"
    sleep 3
}

# ---------------------------------------------------------------------------
printf "\n========================================\n"
printf " HGW-Doctor Integration Tests\n"
printf "========================================\n"
mkdir -p "$DIAG_DIR"
rm -f "$HGW_LOG" "$SVC_LOG"

start_svc
start_hgw

# ---------------------------------------------------------------------------
printf "\n--- Test 1: Process crash detection ---\n"
info "Killing test-service (pid=$SVC_PID)..."
kill "$SVC_PID" 2>/dev/null; wait "$SVC_PID" 2>/dev/null || true
SVC_PID=""

if wait_for_log "Anomaly detected.*type=3\|ANOMALY_PROCESS\|ProcessRestart" 40 "$HGW_LOG"; then
    pass "Process crash anomaly detected"
else
    fail "Process crash anomaly NOT detected within 40s"
fi

if wait_for_log "Recovery dispatched\|restart_process\|ProcessRestart" 5 "$HGW_LOG"; then
    pass "Recovery action dispatched for crashed process"
else
    fail "No recovery action for crashed process"
fi

start_svc
sleep 5

# ---------------------------------------------------------------------------
printf "\n--- Test 2: CPU anomaly detection ---\n"
cpu_stress_start

if wait_for_log "Anomaly detected.*type=1\|ANOMALY_CPU" 60 "$HGW_LOG"; then
    pass "CPU anomaly detected"
else
    fail "CPU anomaly NOT detected within 60s"
fi

cpu_stress_stop
sleep 5

# ---------------------------------------------------------------------------
printf "\n--- Test 3: Memory anomaly detection ---\n"
mem_stress_start

if wait_for_log "Anomaly detected.*type=2\|ANOMALY_MEMORY" 60 "$HGW_LOG"; then
    pass "Memory anomaly detected"
else
    fail "Memory anomaly NOT detected within 60s"
fi

mem_stress_stop

# ---------------------------------------------------------------------------
printf "\n--- Test 4: On-demand diagnostics (SIGUSR1) ---\n"
ARCHIVE_COUNT_BEFORE=$(ls "$DIAG_DIR"/*.tar.gz 2>/dev/null | wc -l)
LOG_DIAG_COUNT_BEFORE=$(grep -c "Diagnostics collected" "$HGW_LOG" 2>/dev/null || echo 0)
info "Sending SIGUSR1 to hgw-doctor (pid=$HGW_PID)..."
kill -USR1 "$HGW_PID"

ELAPSED=0; FOUND=0
while [ $ELAPSED -lt 20 ]; do
    COUNT_NOW=$(grep -c "Diagnostics collected" "$HGW_LOG" 2>/dev/null || echo 0)
    if [ "$COUNT_NOW" -gt "$LOG_DIAG_COUNT_BEFORE" ]; then FOUND=1; break; fi
    sleep 1; ELAPSED=$((ELAPSED+1))
done

if [ $FOUND -eq 1 ]; then
    ARCHIVE_COUNT_AFTER=$(ls "$DIAG_DIR"/*.tar.gz 2>/dev/null | wc -l)
    if [ "$ARCHIVE_COUNT_AFTER" -gt "$ARCHIVE_COUNT_BEFORE" ]; then
        LATEST=$(ls -t "$DIAG_DIR"/*.tar.gz | head -1)
        pass "Diagnostics archive created: $LATEST"
        info "Archive contents:"
        tar -tzf "$LATEST" | sed 's/^/    /'
    else
        fail "Log shows collection but no new archive in $DIAG_DIR"
    fi
else
    fail "No new diagnostics collection logged within 20s"
fi

# ---------------------------------------------------------------------------
printf "\n--- Checking ubus availability for per-process tests ---\n"
UBUS_AVAILABLE=0
if ubus call TestService SetMode '{"mode":"Idle"}' 2>/dev/null; then
    UBUS_AVAILABLE=1
    info "ubus reachable — running per-process anomaly tests"
else
    info "ubus unavailable — skipping Tests 5 & 6"
fi

# ---------------------------------------------------------------------------
printf "\n--- Test 5: Per-process CPU anomaly (ANOMALY_PROCESS_CPU, type=4) ---\n"
if [ $UBUS_AVAILABLE -eq 1 ]; then
    if ! kill -0 "$SVC_PID" 2>/dev/null; then
        start_svc; sleep 5
    fi
    info "Triggering CPUStress via ubus SetMode..."
    ubus call TestService SetMode '{"mode":"CPUStress"}'

    if wait_for_log "Anomaly detected: type=4" 60 "$HGW_LOG"; then
        pass "Per-process CPU anomaly detected (type=4)"
    else
        fail "Per-process CPU anomaly NOT detected within 60s"
    fi

    ubus call TestService SetMode '{"mode":"Idle"}' 2>/dev/null || true
    sleep 5
else
    info "SKIP: ubus unavailable"
fi

# ---------------------------------------------------------------------------
printf "\n--- Test 6: Per-process Memory anomaly (ANOMALY_PROCESS_MEM, type=5) ---\n"
if [ $UBUS_AVAILABLE -eq 1 ]; then
    if ! kill -0 "$SVC_PID" 2>/dev/null; then
        start_svc; sleep 5
    fi
    info "Triggering MemStress via ubus SetMode (allocates 600 MB)..."
    ubus call TestService SetMode '{"mode":"MemStress"}'

    if wait_for_log "Anomaly detected: type=5" 60 "$HGW_LOG"; then
        pass "Per-process Memory anomaly detected (type=5)"
    else
        fail "Per-process Memory anomaly NOT detected within 60s"
    fi

    ubus call TestService SetMode '{"mode":"Idle"}' 2>/dev/null || true
    sleep 5
else
    info "SKIP: ubus unavailable"
fi

# ---------------------------------------------------------------------------
printf "\n========================================\n"
if [ $FAILURES -eq 0 ]; then
    printf "${GREEN}All tests PASSED${NC}\n"
else
    printf "${RED}%d test(s) FAILED${NC}\n" "$FAILURES"
    printf "Log: %s\n" "$HGW_LOG"
fi
printf "Full hgw-doctor log: %s\n" "$HGW_LOG"
printf "========================================\n"
exit $FAILURES
