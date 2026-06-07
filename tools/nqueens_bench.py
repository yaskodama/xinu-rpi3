#!/usr/bin/env python3
"""N-Queens benchmark for the xinu-rpi3 load-balancer.

Three measurement modes for each board size N:

    mac    : full N-Queens on this Mac (Python reference)
    pi3    : delegate every first-column to Pi 3 via /api/loadbal/nqueens
             and poll task results
    split  : Mac handles cols [0 .. N/2), Pi 3 handles cols [N/2 .. N)
             concurrently — this is the "Mac + Xinu distributed" case

Wall-clock is millisecond precision; each mode is run REPEAT times and
the median taken (mean is sensitive to a one-off GC pause on either
side).  Output is JSON to stdout for the LaTeX report builder to
ingest; pass --pretty for human-friendly stderr summary.
"""
import argparse
import concurrent.futures
import json
import statistics
import sys
import time
import urllib.request

PI3_BASE = "http://192.168.3.50:8080"
REPEAT = 3


# ----- reference N-Queens (Mac side) ------------------------------------

def nq_partial(n: int, first_col: int) -> int:
    """Count N-Queens solutions where the row-0 queen sits at first_col."""
    cols = [0] * n
    cols[0] = first_col
    def recurse(row: int) -> int:
        if row == n:
            return 1
        total = 0
        for c in range(n):
            ok = True
            for r in range(row):
                if cols[r] == c or abs(cols[r] - c) == row - r:
                    ok = False
                    break
            if ok:
                cols[row] = c
                total += recurse(row + 1)
        return total
    return recurse(1)


def nq_total(n: int) -> int:
    return sum(nq_partial(n, c) for c in range(n))


# ----- Pi 3 HTTP helpers ------------------------------------------------

def http_get(path: str, timeout: float = 30.0) -> str:
    with urllib.request.urlopen(PI3_BASE + path, timeout=timeout) as r:
        return r.read().decode("utf-8", errors="replace")


def http_post(path: str, timeout: float = 30.0) -> str:
    req = urllib.request.Request(PI3_BASE + path, method="POST")
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.read().decode("utf-8", errors="replace")


def pi3_alive() -> bool:
    try:
        http_get("/api/mmu", timeout=3.0)
        return True
    except Exception:
        return False


def pi3_init_loadbal_and_gc() -> None:
    """The load-balancer + GC actor are no longer auto-started at
    boot; we have to spin them up explicitly before running the
    benchmark.  Both inits are idempotent on the Pi 3 side."""
    http_post("/api/loadbal/init", timeout=5.0)
    http_post("/api/gc-actor/init", timeout=5.0)


def pi3_nq_run(n: int, cols=None, timeout: float = 60.0) -> tuple[int, int]:
    """Submit one N-Queens job per col in `cols` (default 0..n-1), poll
    until every task is DONE/CANCELLED, return (total_count, elapsed_ms)."""
    url = f"/api/loadbal/nqueens?n={n}"
    if cols is not None:
        url += "&cols=" + ",".join(str(c) for c in cols)
    resp = http_post(url, timeout=timeout)
    # Parse "task_id_range=A..B"
    rng = None
    for line in resp.splitlines():
        if "task_id_range=" in line:
            rng = line.split("task_id_range=", 1)[1].strip()
            break
    if rng is None:
        raise RuntimeError(f"Pi 3 did not return task_id_range: {resp!r}")
    a, b = rng.split("..")
    ids = list(range(int(a), int(b) + 1))

    t0 = time.perf_counter()
    total = 0
    pending = set(ids)
    deadline = t0 + max(timeout, 120.0)

    def poll_one(tid: int):
        """Fetch task state.  Returns (tid, state, result) or (tid, None, None)
        on parse/network failure.  Longer timeout because each per-task GET
        through Pi 3's single-thread webactor + slow TCP cycle can be 1-3 s."""
        try:
            body = http_get(f"/api/loadbal/task?id={tid}", timeout=10.0)
        except Exception:
            return tid, None, None
        state = None
        result = None
        for line in body.splitlines():
            # "state=DONE result=4" on one line; pull both fields with
            # independent ifs (was elif, swallowed result on state-match).
            if line.startswith("state="):
                state = line.split("=", 1)[1].split()[0]
            if " result=" in line:
                result = int(line.rsplit("result=", 1)[1].split()[0])
            elif line.startswith("result="):
                result = int(line.split("=", 1)[1].split()[0])
        return tid, state, result

    while pending:
        if time.perf_counter() > deadline:
            raise RuntimeError(f"Pi 3 timeout polling tasks {sorted(pending)}")
        # Fan out the polls — Pi 3 webactor still serializes them, but
        # Mac side doesn't sit on a sequential wait between each.
        with concurrent.futures.ThreadPoolExecutor(
                max_workers=min(4, len(pending))) as pool:
            for tid, state, result in pool.map(poll_one, list(pending)):
                if state == "DONE" and result is not None:
                    total += result
                    pending.discard(tid)
                elif state == "CANCELLED":
                    pending.discard(tid)
        if pending:
            time.sleep(0.1)
    elapsed_ms = int((time.perf_counter() - t0) * 1000)
    return total, elapsed_ms


# ----- modes ------------------------------------------------------------

def mode_mac(n: int) -> tuple[int, int]:
    t0 = time.perf_counter()
    count = nq_total(n)
    return count, int((time.perf_counter() - t0) * 1000)


def mode_pi3(n: int) -> tuple[int, int]:
    return pi3_nq_run(n)


def mode_split(n: int) -> tuple[int, int]:
    """Mac runs cols [0..n/2), Pi 3 runs cols [n/2..n) concurrently."""
    half = n // 2
    mac_cols = list(range(0, half))
    pi3_cols = list(range(half, n))

    def mac_part() -> int:
        return sum(nq_partial(n, c) for c in mac_cols)

    t0 = time.perf_counter()
    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
        f_mac = pool.submit(mac_part)
        f_pi3 = pool.submit(pi3_nq_run, n, pi3_cols)
        mac_count = f_mac.result()
        pi3_count, _ = f_pi3.result()
    elapsed_ms = int((time.perf_counter() - t0) * 1000)
    return mac_count + pi3_count, elapsed_ms


# ----- main -------------------------------------------------------------

MODES = {
    "mac":   mode_mac,
    "pi3":   mode_pi3,
    "split": mode_split,
}


def run_one(mode: str, n: int, repeat: int) -> dict:
    """Run mode `repeat` times for size n, return median+all samples."""
    samples = []
    counts = []
    for _ in range(repeat):
        count, ms = MODES[mode](n)
        samples.append(ms)
        counts.append(count)
    if len(set(counts)) != 1:
        # Mismatched solution counts means a measurement bug or
        # truncated Pi 3 response; flag it loudly in the output.
        return {"mode": mode, "n": n, "samples_ms": samples,
                "counts": counts, "error": "count mismatch"}
    return {"mode": mode, "n": n, "count": counts[0],
            "samples_ms": samples,
            "median_ms": int(statistics.median(samples))}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--sizes", default="8,9,10,11",
                    help="comma-separated N values to benchmark")
    ap.add_argument("--modes", default="mac,pi3,split",
                    help="comma-separated modes to run")
    ap.add_argument("--repeat", type=int, default=REPEAT,
                    help="samples per (mode, n) cell")
    ap.add_argument("--pretty", action="store_true",
                    help="human-readable summary to stderr")
    ap.add_argument("--out", default="-",
                    help="write JSON results to PATH (- = stdout)")
    args = ap.parse_args()

    repeat = args.repeat
    sizes = [int(s) for s in args.sizes.split(",")]
    modes = [m.strip() for m in args.modes.split(",")]

    if any(m in ("pi3", "split") for m in modes):
        if not pi3_alive():
            print("ERROR: Pi 3 at 192.168.3.50:8080 not reachable",
                  file=sys.stderr)
            return 2
        # Spin up load-balancer + GC actor (idempotent).
        pi3_init_loadbal_and_gc()
        if args.pretty:
            print("  Pi 3 loadbal + gc-actor initialized", file=sys.stderr)

    results = []
    for n in sizes:
        for m in modes:
            if args.pretty:
                print(f"  {m}/n={n} ...", end="", file=sys.stderr, flush=True)
            r = run_one(m, n, repeat)
            results.append(r)
            if args.pretty:
                if "error" in r:
                    print(f"  ERROR {r['error']} ({r['samples_ms']} ms)",
                          file=sys.stderr)
                else:
                    print(f"  count={r['count']}  median={r['median_ms']} ms"
                          f"  samples={r['samples_ms']}",
                          file=sys.stderr)

    payload = {
        "host": PI3_BASE,
        "repeat": repeat,
        "results": results,
    }
    out = json.dumps(payload, indent=2)
    if args.out == "-":
        print(out)
    else:
        with open(args.out, "w") as f:
            f.write(out)
        print(f"wrote {args.out}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
