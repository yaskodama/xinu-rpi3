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
MODE_NAMES = {0: "parallel", 1: "staggered(3+2)", 2: "sequential",
              3: "chandy-misra"}


def _http(req_or_url, timeout, retries):
    """Single HTTP call with retry on transient timeouts.  The Pi 3 webactor is
    single-threaded and can briefly stall under load; a lone 10 s timeout used
    to crash the whole bench.  Retry a few times before giving up."""
    last = None
    for attempt in range(retries):
        try:
            with urllib.request.urlopen(req_or_url, timeout=timeout) as r:
                return r.read().decode("utf-8", errors="replace")
        except Exception as e:                       # noqa: BLE001
            last = e
            time.sleep(1.0)
    raise last


def http_get(path, timeout=10.0, retries=4):
    return _http(PI3_BASE + path, timeout, retries)


def http_post(path, timeout=10.0, retries=4):
    req = urllib.request.Request(PI3_BASE + path, method="POST")
    return _http(req, timeout, retries)


def parse_status(body):
    """Pi 3 の /api/dining/status 出力をパース.

    status 行は `elapsed_ms=7950 (final=yes) max_phil_ms=880` のように括弧付き
    トークンを含む。素朴に空白分割すると `(final=yes)` が `k="(final"` になって
    final キーが取れず、完走検知が永久に失敗する (= 旧実装の致命バグ)。
    括弧を剥がしてからパースする。"""
    state = {}
    for line in body.splitlines():
        for tok in line.split():
            tok = tok.strip("()")
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
    """mode/meals でベンチを 1 回走らせ、結果 dict を返す.

    re-init してから start することで、直前 run の final=yes 残骸 (elapsed=0 で
    即終了して見える stale 状態) を避ける。livelock (mode 0 等で 5 人完食しない)
    の場合は timeout まで待ち、その時点の n_done を partial 結果として記録する
    (例外は投げない — livelock 自体がレポートの重要なデータ点)。"""
    http_post("/api/dining/init")          # idempotent: ensure DiningBench exists
    time.sleep(0.4)
    http_post(f"/api/dining/start?mode={mode}&meals={meals}")

    # "started" gate: dining/init does NOT reset the DiningBench result fields
    # (it returns early if the actor already exists), so right after start the
    # status can still show the PREVIOUS run's `final=yes n_done=5/5`.  Wait
    # until run_bench has actually been processed — it stamps the requested
    # mode and resets n_done to 0/final=no — before we trust completion.
    sg = time.time() + 8.0
    while time.time() < sg:
        st = parse_status(http_get("/api/dining/status"))
        if st.get("mode") == mode and st.get("final") != "yes":
            break
        time.sleep(0.2)

    deadline = time.time() + timeout
    st = {}
    while time.time() < deadline:
        st = parse_status(http_get("/api/dining/status"))
        if st.get("final") == "yes" or _ndone(st) >= 5:
            return {"elapsed_ms": st.get("elapsed_ms", -1),
                    "max_phil_ms": st.get("max_phil_ms", -1),
                    "n_done": _ndone(st), "finished": True}
        time.sleep(0.2)
    # timed out -> livelock / no progress; record partial
    return {"elapsed_ms": st.get("elapsed_ms", -1),
            "max_phil_ms": st.get("max_phil_ms", -1),
            "n_done": _ndone(st), "finished": False}


def _ndone(st):
    """status の n_done は "3/5" の形なので分子を取り出す."""
    v = st.get("n_done", 0)
    if isinstance(v, str) and "/" in v:
        try:
            return int(v.split("/", 1)[0])
        except ValueError:
            return 0
    return int(v) if isinstance(v, int) else 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--meals",  type=int, default=50,
                    help="哲学者一人あたりの食事回数 (default 50)")
    ap.add_argument("--repeat", type=int, default=3,
                    help="各 mode の反復数 (median を採用)")
    ap.add_argument("--modes",  default="0,1,2,3",
                    help="比較する mode の comma-list (3=Chandy-Misra)")
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
        ndones = []
        n_finished = 0
        for r in range(args.repeat):
            print(f"  mode {m} ({MODE_NAMES.get(m, '?')}) iter {r+1}/"
                  f"{args.repeat}...", end="", file=sys.stderr, flush=True)
            res = run_one(m, args.meals)
            tag = "DONE 5/5" if res["finished"] else \
                  f"LIVELOCK {res['n_done']}/5"
            print(f" {tag}  elapsed={res['elapsed_ms']}ms  "
                  f"max_phil={res['max_phil_ms']}ms", file=sys.stderr)
            samples.append(res["elapsed_ms"])
            max_phils.append(res["max_phil_ms"])
            ndones.append(res["n_done"])
            if res["finished"]:
                n_finished += 1
        all_finished = (n_finished == args.repeat)
        results.append({
            "mode": m,
            "label": MODE_NAMES.get(m, str(m)),
            "meals": args.meals,
            "samples_ms": samples,
            "median_ms":  int(statistics.median(samples)),
            "max_phil_samples_ms": max_phils,
            "median_max_phil_ms":  int(statistics.median(max_phils)),
            "n_done_samples": ndones,
            "median_n_done":  int(statistics.median(ndones)),
            "finished_runs": n_finished,
            "all_finished":  all_finished,
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
