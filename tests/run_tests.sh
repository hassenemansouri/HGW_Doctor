#!/bin/bash
# =============================================================================
# HGW-Doctor integration test
#
# Tests:
#   1. Process crash detection  — kill test-service, expect ANOMALY_PROCESS
#   2. CPU anomaly detection    — busy-loop, expect ANOMALY_CPU
#   3. Memory anomaly detection — 200 MB alloc, expect ANOMALY_MEMORY
#   4. On-demand diag trigger   — SIGUSR1 → archive created
#
# Requirements:
#   - hgw-doctor and test-service binaries in /tmp/hgw_test/bin/
#   - Run inside the amxdev container (docker exec amxdev bash)
# =============================================================================

set -euo pipefail

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'

pass() { echo -e "${GREEN}[PASS]${NC} $*"; }
fail() { echo -e "${RED}[FAIL]${NC} $*"; FAILURES=$((FAILURES+1)); }
info() { echo -e "${YELLOW}[INFO]${NC} $*"; }

FAILURES=0
HGW_BIN=/tmp/hgw_test/bin/hgw-doctor
SVC_BIN=/tmp/hgw_test/bin/test-service
HGW_CONF=/tmp/hgw_test/conf/hgw_doctor.conf
DIAG_DIR=/tmp/hgw_diag
HGW_LOG=/tmp/hgw_test/hgw-doctor.log
SVC_LOG=/tmp/hgw_test/test-service.log
HGW_PID=""
SVC_PID=""

cleanup() {
    info "Cleaning up..."
    [ -n "$HGW_PID" ] && kill "$HGW_PID" 2>/dev/null && wait "$HGW_PID" 2>/dev/null || true
    [ -n "$SVC_PID" ] && kill "$SVC_PID" 2>/dev/null && wait "$SVC_PID" 2>/dev/null || true
    # Kill any leftover stress processes
    pkill -f "cpu_stress_loop" 2>/dev/null || true
    pkill -f "mem_stress_loop" 2>/dev/null || true
    rm -f /tmp/cpu_stress.py /tmp/mem_stress.py
}
trap cleanup EXIT

wait_for_log() {
    local pattern="$1" timeout="${2:-30}" log="$3"
    local elapsed=0
    while ! grep -q "$pattern" "$log" 2>/dev/null; do
        sleep 1; elapsed=$((elapsed+1))
        if [ $elapsed -ge $timeout ]; then
            return 1
        fi
    done
    return 0
}

start_hgw() {
    info "Starting hgw-doctor..."
    LD_LIBRARY_PATH=/tmp/hgw_test/lib:/usr/lib/x86_64-linux-gnu \
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
    LD_LIBRARY_PATH=/usr/lib/x86_64-linux-gnu \
        "$SVC_BIN" > "$SVC_LOG" 2>&1 &
    SVC_PID=$!
    sleep 1
    if ! kill -0 "$SVC_PID" 2>/dev/null; then
        info "test-service failed to start (ubus likely unavailable) — using shell stub instead"
        # Shell stub: just a named sleeping process
        bash -c 'exec -a test-service sleep 3600' &
        SVC_PID=$!
    fi
    info "test-service running (pid=$SVC_PID)"
}

# ---------------------------------------------------------------------------
echo ""
echo "========================================"
echo " HGW-Doctor Integration Tests"
echo "========================================"
mkdir -p "$DIAG_DIR"
rm -f "$HGW_LOG" "$SVC_LOG"

start_svc
start_hgw

# ---------------------------------------------------------------------------
echo ""
echo "--- Test 1: Process crash detection ---"
info "Killing test-service (pid=$SVC_PID)..."
kill "$SVC_PID" 2>/dev/null; wait "$SVC_PID" 2>/dev/null || true
SVC_PID=""

# hgw-doctor polls every 5s and needs 10s sustained — wait up to 40s
if wait_for_log "Anomaly detected.*type=3\|ANOMALY_PROCESS\|ProcessRestart" 40 "$HGW_LOG"; then
    pass "Process crash anomaly detected"
else
    fail "Process crash anomaly NOT detected within 40s"
fi

# Check recovery action was triggered
if wait_for_log "Recovery dispatched\|restart_process\|ProcessRestart" 5 "$HGW_LOG"; then
    pass "Recovery action dispatched for crashed process"
else
    fail "No recovery action for crashed process"
fi

# Restart test-service for next tests
start_svc
sleep 5  # let monitor see it alive again

# ---------------------------------------------------------------------------
echo ""
echo "--- Test 2: CPU anomaly detection (threshold=60%, duration=10s) ---"
info "Launching CPU stress (busy-loop on all cores)..."

# Python busy loop — one process per CPU
NCPU=$(nproc)
for i in $(seq 1 $NCPU); do
python3 -c "
import time
end = time.time() + 90
while time.time() < end:
    pass
" &
done

CPU_STRESS_PIDS=$(pgrep -f "import time" | tr '\n' ' ')
info "CPU stress pids: $CPU_STRESS_PIDS"

if wait_for_log "Anomaly detected.*type=1\|ANOMALY_CPU" 60 "$HGW_LOG"; then
    pass "CPU anomaly detected"
else
    fail "CPU anomaly NOT detected within 60s"
fi

# Stop CPU stress
kill $CPU_STRESS_PIDS 2>/dev/null || true
wait $CPU_STRESS_PIDS 2>/dev/null || true
info "CPU stress stopped"
sleep 5

# ---------------------------------------------------------------------------
echo ""
echo "--- Test 3: Memory anomaly detection (threshold=70%) ---"
TOTAL_MEM_KB=$(grep MemTotal /proc/meminfo | awk '{print $2}')
TOTAL_MEM_MB=$((TOTAL_MEM_KB / 1024))
ALLOC_MB=$(( (TOTAL_MEM_MB * 75) / 100 ))
info "System RAM: ${TOTAL_MEM_MB} MB — allocating ${ALLOC_MB} MB (75%)..."

python3 -c "
import time, ctypes
sz = $ALLOC_MB * 1024 * 1024
buf = ctypes.create_string_buffer(sz)
ctypes.memset(buf, 0xAA, sz)
print('MemStress: ${ALLOC_MB}MB allocated')
time.sleep(90)
" &
MEM_STRESS_PID=$!
info "Memory stress pid=$MEM_STRESS_PID"

if wait_for_log "Anomaly detected.*type=2\|ANOMALY_MEMORY" 60 "$HGW_LOG"; then
    pass "Memory anomaly detected"
else
    fail "Memory anomaly NOT detected within 60s"
fi

kill $MEM_STRESS_PID 2>/dev/null; wait $MEM_STRESS_PID 2>/dev/null || true
info "Memory stress stopped"
sleep 5

# ---------------------------------------------------------------------------
echo ""
echo "--- Test 4: On-demand diagnostics (SIGUSR1) ---"
ARCHIVE_COUNT_BEFORE=$(ls "$DIAG_DIR"/*.tar.gz 2>/dev/null | wc -l)
LOG_DIAG_COUNT_BEFORE=$(grep -c "Diagnostics collected" "$HGW_LOG" 2>/dev/null || echo 0)
info "Sending SIGUSR1 to hgw-doctor (pid=$HGW_PID)..."
kill -USR1 "$HGW_PID"

# Wait for a NEW "Diagnostics collected" log line (count must increase)
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
echo ""
echo "========================================"
if [ $FAILURES -eq 0 ]; then
    echo -e "${GREEN}All tests PASSED${NC}"
else
    echo -e "${RED}$FAILURES test(s) FAILED${NC}"
    echo "Log: $HGW_LOG"
fi
echo "Full hgw-doctor log: $HGW_LOG"
echo "========================================"
exit $FAILURES
