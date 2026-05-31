# NEXT_SESSION — xinu-raz / arm-rpi3 (Raspberry Pi 3 B+ 実機移植)

最終更新: 2026-06-01 / ブランチ `arm-rpi3-port` / repo `github.com/yaskodama/xinu-rpi3`

## 現状サマリ ✅

実機 **Raspberry Pi 3 B+ (BCM2837)** 上で 32-bit Embedded Xinu が完全動作:

- ✅ 起動 + `xsh$` 対話シェル (シリアル UART0 / HDMI 両方)
- ✅ HDMI フレームバッファ + 窓システム (Pi5 から移植) + USB キーボード/マウス
- ✅ **USB ethernet (LAN78xx)** — ping ~3ms, TCP 双方向動作
- ✅ **AIPL ランタイム** (philosopher/web/dispatcher/worker/collector で 7 actor 並列)
- ✅ **HTTP server (port 8080)** — 25+ ルートで introspection + 操作
- ✅ **AIPL-RPC server (port 5555)** — Mac から SPAWN/COMPILE 等
- ✅ **C JIT (cc_mvp Stage 5)** — Turing-complete に近い structured C を ARM32 機械語に変換+実行
- ✅ **負荷分散アクター** — Dispatcher + 4 Workers, sleep & JIT 両モード, pause/resume + per-task tracking + cancellation + 429 backpressure + latency histogram + **sticky routing + worker restart + rate limit + task timeout + priority (Tier 2 完)**
- ✅ **Global actor GC (Collector actor)** — 周期 sweep、infrastructure protect、HTTP で configure/enable/sweep_now、task-timeout scan も担当
- ✅ **MMU enable** — identity-map, RAM/MMIO 領域属性分離
- ✅ **Network kernel update** — `/upload` + `/kexec` で SD swap 不要のカーネル入れ替え
- ✅ TCP upload ~26 KB/s (IBLEN bump 後)

**2026-05-27**: ethernet ping 完全動作 (LAN78xx ドライバ + DWC NAK 修正)
**2026-06-01**: AIPL/JIT/loadbal/MMU/kexec/TCP 高速化 + loadbal Tier 1+2 + GC actor を一気に積み上げ (34 commits)

## 2026-06-01 セッション概要 (34 commits)

```
3f9fb9e  cc_mvp MVP — int main() { return CONST; }
4a5d67a  + binop (+ -)
f9384f8  + function call + builtin syms
(introspection routes, /upload, /gc, /mmio-read, age tracking 等)
bf7575e  load-balancer actors (Dispatcher + 4 Workers, least-loaded routing)
d8d3c2c  + JIT × loadbal 統合 (compute_jit, round-robin, memfree leak fix)
19c5050  cc_mvp Stage 4 — locals + multi-stmt + if/else
e0cfc09  cc_mvp Stage 5 — comparison operators + while loops
4d98e95  NEXT_SESSION URL update (GitHub repo rename)
4979d0a  MMU enable — identity-map + region attributes (RAM vs MMIO)
93f3f95  dnsmasq path + version.h refresh
84f47c0  /kexec — network kernel update via HTTP, no SD swap
72495d5  /upload chunked body reader + TCP_GRACIOUSACK (1.37×)
451fd15  TCP_IBLEN 16 KB → 65528 (5.4× upload speedup, fix ushort wrap)
b2e0137  NEXT_SESSION refresh (2026-06-01 24 commits)
fbcc192  README English fork overview
b7e5b5e  loadbal — pause/resume + per-task tracking + enabled mask
5e63d31  loadbal Tier 1 — cancellation + 429 backpressure + latency histogram
11cd126  global actor GC implemented as actor (CLASS_Collector)
9c89aad  version.h refresh
8d1ef4b  NEXT_SESSION refresh (30 commits 全反映)
53d29bd  loadbal Tier 2 — sticky routing + worker restart + prefix-collision fix
50b3366  loadbal Tier 2 rest — rate limit + task timeout + priority
456d247  version.h refresh                                       ← 最新
```

## 重要な追加機能 (このセッション)

### C JIT (`apps/cc_mvp.c`) — Stage 5
```c
// 動作する例 — Turing-complete C subset:
int main() {
  int s = 0; int i = 1;
  while (i <= 10) { s = s + i; i = i + 1; }
  return s;          // -> 55 (140 B of ARM32 code)
}
```

サポート: `int main()` + locals + `+ - == != < > <= >= = if/else while return` + builtin call。

Builtin symbol 表 (`cc_syms[]`): `print_int`, `actor_count`, `actor_age(id)`, `now_ms`。
新規 builtin 追加 = テーブルに 1 行追加するだけ。

ARM32 codegen: identity-mapped RAM 上に `memget(4096)` → ARM32 命令を直接書き込み → 関数ポインタ
キャストして即実行 → `memfree(4096)` (leak fix 済)。

エントリ: `int cc_mvp_compile_and_run(const char *src, long *retval, int *codesize)`
HTTP: `POST /compile` with C source as body。

### 負荷分散アクター (`apps/abcl_program.c`)
- `CLASS_Dispatcher` (3) — 最少負荷 worker に `compute` 転送 (sleep mode)、round-robin で
  `compute_jit` 転送 (JIT mode)
- `CLASS_Worker` (4) × 4 — sleep または JIT で並列処理、結果を `done` で dispatcher に返却
- `LB_N_WORKERS = 4` (MAX_OBJECTS=16 のうち bootstrap 7 + 余裕9)

**Tier 1 機能 (b7e5b5e + 5e63d31)**:
- **Pause/resume drain**: `set_enabled(idx, on)` で worker 一時停止、dispatcher が skip。
  `F_Disp_enabled` mask field (bit i = worker i 有効) で管理。in-flight タスクは完了する。
- **Per-task tracking**: `lb_tasks[64]` circular buffer に submit_ms/done_ms/result/state 保持。
  Mac は fire-and-forget して後で `/api/loadbal/task?id=N` で query 可能。
- **Cancellation**: `cancel_task(id)` で state を CANCELLED に。Worker は chunked sleep (50ms 毎)
  で state を polling、abort 可能。JIT は pre-flight check のみ (compile+exec は同期)。
- **429 backpressure**: `LB_WORKER_QUEUE_MAX=8` 超えた worker しか無ければ `/submit` /`jit` は
  HTTP 429 + `Retry-After: 1` 返却 (mailbox 飽和を防ぐ)。
- **Latency histogram**: Worker毎 7 bucket (<50/50-99/100-199/200-499/500-999/1k-2k/≥2k ms)。
  Dispatcher_done が done時に elapsed_ms 計算 → 該当 bucket を増分 → `/api/loadbal/stats` で表示。

**Tier 2 機能 (53d29bd + 50b3366)**:
- **Sticky routing**: `/api/loadbal/submit-sticky?key=NAME` で djb2(key)%n で同 worker 集中。
  paused なら次の enabled に walk。実機: key=foo 8 タスク全部 worker 1 (hash=193491849%4=1)。
- **Worker restart**: `/api/loadbal/restart?w=N` で kill+alloc+spawn+bind+slot 更新。新 obj_id
  (n_objects bump)。実機: w=1 → obj 3 dead, obj 7 新規割当、後続 submit が obj 7 に届く。
- **Rate limiting**: token bucket (file-scope state)、`/api/loadbal/rate-limit?per_sec=N&capacity=M`
  で構成、`/api/loadbal/rate-stats` で query。`/submit` と `/jit` が gate (queue cap の後で)。
- **Task timeout**: `/api/loadbal/timeout?ms=N` (0=disable、default 30000)。Collector.tick が
  毎秒 PENDING タスクを scan、age > timeout なら state=CANCELLED + g_task_timed_out++。
- **Priority (simplified)**: `?prio=high` で **rate-limit bypass** (queue cap は引き続き check)。
  真の priority queue (per-prio mailbox + preemption) は Tier 3 に deferred。

**Tier 2 prefix-collision バグ fix**: `/api/loadbal/submit` (23 chars) が `/api/loadbal/submit-sticky`
にも match して sticky が non-sticky path に落ちていた → 次の char が `?`/` ` の guard 追加で修正。

### Global Actor GC (`apps/abcl_program.c` — CLASS_Collector)
- `CLASS_Collector` (5) — actor として実装された GC、独自 mailbox + dispatch loop
- **Heartbeat thread** (`abcl_gc_heartbeat`) — 1s 周期で Collector に "tick" message 送信
- Collector.tick は `period_ms` 経過判定 → `abcl_gc_sweep(threshold_ms)` 呼び出し
- Fields: period_ms (default 5000)、threshold_ms (default 30000)、enabled、sweep_count、
  swept_total、last_swept_n、last_scanned_n、last_sweep_ticks
- Methods: tick / configure / enable / sweep_now / init
- Priority `ABCL_PRIO_Collector=26` (Dispatcher 25 より高い、starve 防止)
- Bootstrap (`abcl_gc_actor_init`) で WebReceiver/Dispatcher/Workers/Collector を全て
  `abcl_object_protect(1)` に → mis-set threshold で runtime を reap 不能
- HTTP: `/api/loadbal/submit?n=K&ms=M`, `/api/loadbal/jit?n=K&prog=P`, `/api/loadbal/stats`
- stress test 実績: 256 sleep tasks (4 burst) + 64 JIT tasks、drops=0、各 worker 完全均等分散

### MMU (`system/platforms/arm-rpi3/mmu.c`) ★慎重要
- ARMv7-A short-descriptor、16KB-aligned L1 page table (4096 sections × 1MB)
- Identity map (VA==PA):
  - 0x00000000 .. 0x3EFFFFFF: RAM Normal cacheable RWX (TEX=001 C=1 B=1)
  - 0x3F000000 .. 0x3FFFFFFF: MMIO Device strongly-ordered RW-NX
  - 0x40000000 .. 0x40FFFFFF: ARM local regs Device RW-NX
- **SCTLR.M=1 のみ (C/I/Z は触らない)** — M+C+I+Z 同時 enable は Cortex-A53 で brick 実績あり
  ([Pi 3 MMU enable hazard](../../.claude/projects/-Users-kodamay/memory/feedback-xinu-rpi3-mmu-enable-hazard.md) に記録)
- `mmu_disable()` を kexec 前に呼ぶ必要あり (新 kernel start.S は MMU off 前提)
- HTTP: `/api/mmu` で SCTLR/TTBR0/有効状態を確認

### Network kernel update (`/kexec`)
Mac → Pi 3 物理操作なしの kernel 更新ループ:
```bash
curl --data-binary @xinu.boot \
     "http://192.168.3.50:8080/upload?dst=xinu.boot"   # ~11s for 305 KB
curl -X POST http://192.168.3.50:8080/kexec            # jump
# ~20s wait
curl http://192.168.3.50:8080/api/mmu                  # 新 kernel 稼働確認
```

Pi 3 内部: `mmu_disable()` → `kexec()` stub at 0x7FE8 → upload を 0x8000+ にコピー → `mov pc, #0x8000`
→ 新 kernel start.S → `platforminit` → `mmu_init` (MMU 再 enable) → `webactor_autostart`

**Volatile**: 電源 OFF で SD 再 load に戻る (永続化は要 SDHOST driver port)。

### TCP 高速化 (`include/tcp.h`)
- `TCP_IBLEN`: 16384 → **65528** (65535 ushort cap 直下、8の倍数)
  - **65536 は brick** (`tcpSendWindow.c:24` の `ushort window = TCP_IBLEN - icount` で wrap → window=0
    advertise → 全 TCP 死亡)
- `TCP_GRACIOUSACK`: define (drain 時の window update ack を送って sender 停止を解除)
- Upload `305 KB`: 91s → **11.5s** (~8× 累計、26 KB/s)

## 主要 HTTP routes (port 8080)

```
GET  /                                 — bare-/ delivers to AIPL web_receiver
POST /upload?dst=NAME                  — store request body in upload slot (512 KB max)
GET  /api/upload-info                  — last upload metadata + first 32 B hex
POST /kexec                            — jump to last /upload (network kernel update)
GET  /api/mmu                          — MMU SCTLR/TTBR0 dump

# Actor introspection
GET  /api/actors                       — AIPL actor inventory (id class tid started dead enq deq drops)
GET  /api/actor-age?id=N               — ms since last mailbox activity
POST /api/actor-kill?id=N              — kill an actor
POST /gc?threshold_ms=N[&dry=0|1]      — one-shot sweep (manual, on webactor thread)
POST /api/actor-send?to=N&m=METHOD&v=  — inject message into any actor's mailbox
GET  /api/object-field?id=N&field=K    — peek actor field
GET  /api/threads                      — thrtab dump
GET  /api/memstat                      — free bytes

# Load-balancer (基本 + Tier 1)
GET  /api/loadbal/stats                — dispatcher + workers + enabled_mask + per-worker histogram
POST /api/loadbal/submit?n=K&ms=M[&prio=high]      — submit K sleep tasks (429 on cap+rate)
POST /api/loadbal/jit?n=K&prog=P[&prio=high]       — submit K JIT tasks (429 on cap+rate)
POST /api/loadbal/pause?w=N            — pause worker N (drain mode)
POST /api/loadbal/resume?w=N           — resume worker N
GET  /api/loadbal/task?id=N            — query task by id (last 64 retained)
POST /api/loadbal/cancel?id=N          — mark task as cancelled (worker aborts on chunk boundary)

# Load-balancer Tier 2
POST /api/loadbal/submit-sticky?n=K&ms=M&key=NAME  — djb2(key)%n で同 worker 集中
POST /api/loadbal/restart?w=N          — kill + alloc + spawn + bind (n_objects bump)
POST /api/loadbal/rate-limit?per_sec=N&capacity=M  — token bucket configure
GET  /api/loadbal/rate-stats           — tokens / throttled / timed_out counters
POST /api/loadbal/timeout?ms=N         — server-side PENDING task deadline (0=disable, default 30000)
#                                        priority: ?prio=high が submit/jit で rate-limit bypass

# GC actor (periodic global actor sweep)
GET  /api/gc-actor/stats               — period/threshold + sweep_count + counters
POST /api/gc-actor/configure?period=N&threshold=M
POST /api/gc-actor/enable?on=0|1
POST /api/gc-actor/sweep_now           — force immediate sweep

# JIT + misc
POST /compile                          — compile + run C source (body = source)
GET  /mmio-read?addr=0xN               — peek 32-bit register
POST /reboot                           — watchdog reset (re-load from SD)
GET  /sd-test                          — read LBA 0 (doesn't currently work — controller mismatch)
```

## カーネル更新フロー — 2 通り

### A) Network update (推奨、~11s + ~20s = 30s 程度)
```bash
make PLATFORM=arm-rpi3   # in compile/
curl --data-binary @xinu.boot "http://192.168.3.50:8080/upload?dst=xinu.boot"
curl -X POST http://192.168.3.50:8080/kexec
sleep 25
curl http://192.168.3.50:8080/api/mmu   # 動作確認
```

### B) SD swap (brick 復旧 + 永続化)
```bash
# Pi 3 から SD 取り出し → Mac に挿入 → /Volumes/XINU mount
cp xinu.boot /Volumes/XINU/kernel.img
cp xinu.boot /Users/kodamay/tftpboot/kernel.img    # tftp も同期しておく
sync && diskutil eject /Volumes/XINU
# SD を Pi 3 に戻して電源 ON
```

backup kernel:
- `/Users/kodamay/tftpboot/kernel.img.pre-mmu-bak` (304648 B、MMU 無し、究極の fallback)
- `/Users/kodamay/tftpboot/kernel.img.pre-kexec-bak` (305224 B、MMU only、kexec 無し)
- `/Users/kodamay/tftpboot/kernel.img` (現行 build)

## 既知の制約 / 次の候補作業

### 高優先
- **D-cache 有効化** — 現状 `SCTLR.C=0`、cacheable RAM 属性は inert。Cortex-A53 D-cache set/way
  invalidate loop (4-way × 128 set × 64 B 一括 DCISW) を mmu.c に追加 → `SCTLR.C=1` で性能向上
- **I-cache 有効化** — 同様に ICIALLU 後 `SCTLR.I=1`。これも性能寄与大

### 中優先
- **SDHOST driver port** — BCM2837 のデフォルト SD コントローラ (EMMC でなく SDHOST @ 0x3F202000)
  → 現 `apps/sd_block.c` は EMMC 前提で動かない。永続的な network kernel write には必須
- **JIT 拡張** — 論理 (`&&`/`||`), unary, ポインタ/配列、関数定義 (1関数→複数関数)、
  value_t builtin (v_int_of, v_add, v_eq 等) → aipl2c 出力を JIT で実行できる
- **Load-balancer Tier 2 残機能の実機テスト** (このセッションで未実施):
  - rate-limit (`/api/loadbal/rate-limit` + `/rate-stats`)、task timeout、prio=high bypass
  - Code は commit 50b3366 で実装+build success、deploy 時 USB ethernet brick で実機確認断念
  - 次セッションで SD-direct flash 後に検証
- **Load-balancer Tier 3 機能**:
  - true priority queues (per-prio mailbox + preemption)、auto-retry with task-id lineage、
    dynamic worker pool resize (MAX_OBJECTS=16 cap)、speculative execution、scatter-gather、
    worker affinity / session sticky beyond hash

### 低優先 / 未着手
- per-process VAS + TTBR0 swap (要 mmu_disable + context save 拡張)
- demand paging (要 SD block I/O + fault handler)
- USB エンドポイントを増やす (`MAX_RX_REQUESTS=1` のまま)、高スループット時の挙動評価
- ICMP echo 応答は `icmpDaemon` (prio 30) 経由非同期、負荷時挙動未評価
- telnet pseudo device の子スレッド printf 欠落問題は前回未解決 (このセッションでは未着手)

### 永続性の注釈
- `/kexec` は volatile — 電源 OFF で SD reload に戻る
- SD への書き込みは SDHOST driver 完成までは物理 swap 必須

## 重要なファイル (このセッションで追加/変更)

```
apps/cc_mvp.c                            — C JIT (Stage 5)
apps/abcl_program.c                      — + Dispatcher / Worker / Collector
                                           + lb_tasks tracking + histogram + GC actor
apps/webactor.c                          — + 多数の /api routes, /kexec, chunked reader
                                           + 429 backpressure + /api/gc-actor/*
apps/sd_block.c                          — SD driver (動かない、SDHOST が必要)
system/platforms/arm-rpi3/mmu.c          — MMU enable/disable
system/platforms/arm-rpi3/Makerules      — + mmu.c
system/platforms/arm-rpi3/platforminit.c — + mmu_init() 呼び出し
include/tcp.h                            — TCP_IBLEN=65528 + TCP_GRACIOUSACK
```

## 過去セッションの記録 (2026-05-27、ethernet + telnet)

下記は前回セッションの作業ログ。**全て解決済 or 別 issue** として残置。

### コミット (PR #1 / `arm-rpi3-port` の ethernet 系)
```
a49ed86  arm-rpi3: fix ~10s ping RTT — bulk endpoints must defer fast on NAK
a020953  arm-rpi3: fix LAN78xx ping — MAF offset, burst cap, fixed MAC, async open
9d13796  arm-rpi3: re-enable ETH0 open at boot
7aeec1e  arm-rpi3: LAN78xx (LAN7515) USB ethernet driver
c6ecdc1  arm-rpi3: window system (ported from Pi5) + USB mouse + `win`
61cb663  arm-rpi3: USB (DWC OTG) bring-up + HDMI keyboard shell
1837e74  arm-rpi3: enable HDMI framebuffer output
```

### ethernet が動かなかった 3 つの真因 (動作する設定)

1. **`BURST_CAP=0` で CPU 飢餓** — `HW_CFG_MEF` + `USB_CFG_BIR` + `BURST_CAP=0x25` 必須
2. **MAF レジスタオフセット誤り** — `MAF_HI_0/LO_0` は `0x400/0x404` (not 0x150/0x154)
3. **bulk NAK defer の UB** — `(1 << (bInterval-1))` で bInterval=0 だと UB → 巨大 defer。
   bulk/control は defer=1ms 固定

### LAN78xx 確定構成
- `RFE_CTL = BCAST_EN(0x400) | DA_PERFECT(0x2)` (no UCAST_EN)
- `HW_CFG |= MEF(0x10)`、`USB_CFG0 |= BIR(0x40)`
- `BURST_CAP = 0x25`、`BULK_IN_DLY = 0x800`
- `FCT_RX_FIFO_END=0x27, FCT_TX_FIFO_END=0x11`
- MAF[0] = 自分の MAC @ 0x400/0x404 + `MAF_HI_AF_EN=0x80000000`
- VID 0x0424 / PID 0x7800、bRequest 0xA0(write)/0xA1(read)
- 固定 MAC `b8:27:eb:c0:ff:ee`

### telnet pseudo device の未解決問題
子スレッド (xsh_help 等) の printf 出力がクライアントに届くのが間欠的。
シェルスレッド出力は届く。`fdesc[1]=26=telnetdev` 設定は正しいのに `telnetWrite` に到達しない。
- 関連: `telnet.h`, `device/telnet/telnetWrite.c`, `telnetRead.c`, `telnetServer.c`
- `TELNET_TRACE` で `telnet.h` の `//#define TRACE_TELNET 1` 有効化可能
- 次の手: `xsh_help` に「コマンド一覧 printf 到達 + その時の gettid()/stdout」トレース、
  `fputc/putc` (lib/libxc) に子スレッド書き込みトレース

## 実機テスト手順 + 落とし穴

### Build + Flash
```bash
cd compile && make PLATFORM=arm-rpi3   # → xinu.boot (~305 KB)
# Network update なら /upload + /kexec (上記 A)
# Physical なら SD swap (上記 B)
```

### シリアル読み取り (Python termios, B115200)
- macOS USB-TTL は再接続で**デバイス名が変わる** (`-120`↔`-1120`)。
  毎回 `ls /dev/cu.usbserial-*` 確認
- 送信は char-by-char (~25-30ms/char) で送る (Pi UART RX ドロップ回避)
- `rxf#` 等 ISR からの出力が出ても**シェル応答とは限らない**。応答確認は `help` 等でエコー確認
- 前回分が掴んでいたら `pkill -f usbserial`

### ネットワーク
- Pi はルータ LAN、Mac は同ルータ WiFi (192.168.3.202)、同一 /24
- Pi 静的 IP: `192.168.3.50` (`netup` は自動)
- Mac から ping `192.168.3.50`
- パケット観察: `sudo tcpdump -ni en0 host 192.168.3.50`

### HTTP 経由のデバッグ (推奨)
シリアル不安定なので HTTP 経由が現実的:
- `/api/actors`, `/api/threads`, `/api/memstat` で実行時状態
- `/api/mmu` で MMU 状態
- `/api/loadbal/stats` で負荷分散
- `/compile` で JIT 動作確認

### kexec brick 復旧
1. Pi 3 電源 OFF (コードも数秒抜く — USB controller 放電)
2. SD swap → backup kernel 焼く
3. 電源 ON → 復旧確認

## メモリ (~/.claude/.../memory/)
- `project-xinu-raz-pi3-evolution.md` — このプロジェクト全体
- `feedback-xinu-rpi3-mmu-enable-hazard.md` — MMU enable で SCTLR.M only にする理由 (brick 教訓)
- `feedback-xinu-rpi4-flash-verify-workflow.md` — Pi 4 用だが思想は共通

## 関連リポジトリ
- `xinu-rpi5` (Pi5 AArch64、別 repo) — 窓システムの移植元
- `xinu-rpi4` (Pi 4 AArch64、別 repo) — AIPL ランタイムの上流、機能パリティの参照先
