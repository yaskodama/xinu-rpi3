#!/usr/bin/env python3
"""5 哲学者問題の分散戦略ベンチマーク driver (Mac 側)。

3 つの戦略を順に走らせ、各々の壁時計時間 (Pi 3 側計測) を JSON に出力する:
    mode 0  全 5 並列 (古典 dining philosophers)
    mode 1  3+2 段階  (P0..P2 → 完了後 P3,P4)
    mode 2  順次 1 人ずつ  (P0 → P1 → P2 → P3 → P4)

期待される傾向: 並列性が高いほど壁時計が短い (フォーク争奪以外は同時進行)。
mode 0 < mode 1 < mode 2 になるはず。
"""
import argparse
import json
import statistics
import sys
import time
import urllib.request

PI3_BASE = "http://192.168.3.50:8080"
MODE_NAMES = {0: "parallel", 1: "staggered(3+2)", 2: "sequential"}


def http_get(path, timeout=10.0):
    with urllib.request.urlopen(PI3_BASE + path, timeout=timeout) as r:
        return r.read().decode("utf-8", errors="replace")


def http_post(path, timeout=10.0):
    req = urllib.request.Request(PI3_BASE + path, method="POST")
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.read().decode("utf-8", errors="replace")


def parse_status(body):
    """Pi 3 の /api/dining/status 出力をパース."""
    state = {}
    for line in body.splitlines():
        for tok in line.split():
            if "=" in tok:
                k, v = tok.split("=", 1)
                try:
                    state[k] = int(v)
                except ValueError:
                    state[k] = v
    return state


def init_pi3():
    """GC actor + dining actor を起動 (idempotent)."""
    http_post("/api/gc-actor/init")
    http_post("/api/dining/init")


def run_one(mode, meals, timeout=60.0):
    """mode/meals でベンチを 1 回走らせ、壁時計 ms と max_phil_ms を返す."""
    http_post(f"/api/dining/start?mode={mode}&meals={meals}")
    deadline = time.time() + timeout
    last_status = ""
    while time.time() < deadline:
        body = http_get("/api/dining/status")
        st = parse_status(body)
        last_status = body
        if st.get("final") == "yes" or st.get("n_done") == 5:
            return st.get("elapsed_ms", -1), st.get("max_phil_ms", -1), st
        time.sleep(0.2)
    raise TimeoutError(f"dining mode={mode} did not finish in {timeout}s; "
                       f"last status: {last_status!r}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--meals",  type=int, default=50,
                    help="哲学者一人あたりの食事回数 (default 50)")
    ap.add_argument("--repeat", type=int, default=3,
                    help="各 mode の反復数 (median を採用)")
    ap.add_argument("--modes",  default="0,1,2",
                    help="比較する mode の comma-list")
    ap.add_argument("--out",    default="-", help="JSON 出力先 (- = stdout)")
    args = ap.parse_args()

    try:
        init_pi3()
    except Exception as e:
        print(f"ERROR: Pi 3 init failed: {e}", file=sys.stderr)
        return 2

    modes   = [int(x) for x in args.modes.split(",")]
    results = []
    for m in modes:
        samples = []
        max_phils = []
        for r in range(args.repeat):
            print(f"  mode {m} ({MODE_NAMES.get(m, '?')}) iter {r+1}/"
                  f"{args.repeat}...", end="", file=sys.stderr, flush=True)
            elapsed_ms, max_phil_ms, st = run_one(m, args.meals)
            print(f" elapsed={elapsed_ms}ms  max_phil={max_phil_ms}ms",
                  file=sys.stderr)
            samples.append(elapsed_ms)
            max_phils.append(max_phil_ms)
        results.append({
            "mode": m,
            "label": MODE_NAMES.get(m, str(m)),
            "meals": args.meals,
            "samples_ms": samples,
            "median_ms":  int(statistics.median(samples)),
            "max_phil_samples_ms": max_phils,
            "median_max_phil_ms":  int(statistics.median(max_phils)),
        })

    payload = {
        "host":   PI3_BASE,
        "meals":  args.meals,
        "repeat": args.repeat,
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
