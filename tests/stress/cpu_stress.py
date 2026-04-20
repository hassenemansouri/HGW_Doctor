#!/usr/bin/env python3
"""
cpu_stress.py - stress tool for hgw-doctor testing.

Usage:
  python3 cpu_stress.py cpu   [duration_s]   saturate CPU for N seconds (default: 120)
  python3 cpu_stress.py mem   [mb]           allocate N MB and hold (default: 200)
  python3 cpu_stress.py both  [duration_s]   CPU + memory together
  python3 cpu_stress.py idle                 just stay alive (baseline / process-liveness test)

Examples:
  python3 cpu_stress.py cpu 90    # hold CPU above threshold for 90s -> triggers anomaly
  python3 cpu_stress.py mem 300   # alloc 300 MB              -> triggers memory anomaly
  python3 cpu_stress.py idle      # stay as a named process   -> kill it to test restart
"""

import sys
import time
import threading
import multiprocessing

# --------------------------------------------------------------------------- #
# Helpers
# --------------------------------------------------------------------------- #

def _cpu_burner(stop_event):
    """Single-threaded busy loop - run one per core to saturate the system."""
    while not stop_event.is_set():
        pass


def stress_cpu(duration_s: int):
    n_cores = multiprocessing.cpu_count()
    print(f"[cpu_stress] burning {n_cores} core(s) for {duration_s}s …", flush=True)
    stop = threading.Event()
    threads = [threading.Thread(target=_cpu_burner, args=(stop,), daemon=True)
               for _ in range(n_cores)]
    for t in threads:
        t.start()
    time.sleep(duration_s)
    stop.set()
    for t in threads:
        t.join()
    print("[cpu_stress] CPU stress done.", flush=True)


def stress_mem(mb: int):
    print(f"[cpu_stress] allocating {mb} MB and holding (Ctrl-C to release) …", flush=True)
    # Allocate and *touch* every page so the kernel actually maps it.
    chunk = bytearray(mb * 1024 * 1024)
    for i in range(0, len(chunk), 4096):
        chunk[i] = 0xAA
    print(f"[cpu_stress] {mb} MB allocated. Sleeping …", flush=True)
    try:
        while True:
            time.sleep(60)
    except KeyboardInterrupt:
        pass
    print("[cpu_stress] memory released.", flush=True)


def idle():
    print("[cpu_stress] idle mode — process is alive (kill -9 me to test restart).", flush=True)
    try:
        while True:
            time.sleep(10)
    except KeyboardInterrupt:
        pass


# --------------------------------------------------------------------------- #
# Entry point
# --------------------------------------------------------------------------- #

def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else "idle"

    if mode == "cpu":
        duration = int(sys.argv[2]) if len(sys.argv) > 2 else 120
        stress_cpu(duration)

    elif mode == "mem":
        mb = int(sys.argv[2]) if len(sys.argv) > 2 else 200
        stress_mem(mb)

    elif mode == "both":
        duration = int(sys.argv[2]) if len(sys.argv) > 2 else 120
        mb       = int(sys.argv[3]) if len(sys.argv) > 3 else 200
        t = threading.Thread(target=stress_mem, args=(mb,), daemon=True)
        t.start()
        stress_cpu(duration)

    elif mode == "idle":
        idle()

    else:
        print(__doc__)
        sys.exit(1)


if __name__ == "__main__":
    main()
