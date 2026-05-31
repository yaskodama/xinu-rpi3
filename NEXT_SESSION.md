# NEXT_SESSION — xinu-raz / arm-rpi3 (Raspberry Pi 3 B+ 実機移植)

最終更新: 2026-06-01 / ブランチ `arm-rpi3-port` / repo `github.com/yaskodama/xinu-rpi3`

## 現状サマリ ✅

実機 **Raspberry Pi 3 B+ (BCM2837)** 上で 32-bit Embedded Xinu が完全動作:

- ✅ 起動 + `xsh$` 対話シェル (シリアル UART0 / HDMI 両方)
- ✅ HDMI フレームバッファ + 窓システム (Pi5 から移植) + USB キーボード/マウス + `win` コマンド
- ✅ **USB ethernet (LAN78xx = LAN7515 オンボード NIC) 完全動作**:
  link / RX / TX / ARP / ICMP / **ping が正常レイテンシ (Mac→Pi ~10ms / 0% loss)**

**この回 (2026-05-27) で ethernet の ping を一から動かしきった。** 下記「今回の修正連鎖」参照。

## コミット (PR #1 / `arm-rpi3-port`)

```
a49ed86  arm-rpi3: fix ~10s ping RTT — bulk endpoints must defer fast on NAK  ← 最新
a020953  arm-rpi3: fix LAN78xx ping — MAF offset, burst cap, fixed MAC, async open
9d13796  arm-rpi3: re-enable ETH0 open at boot
7aeec1e  arm-rpi3: LAN78xx (LAN7515) USB ethernet driver
c6ecdc1  arm-rpi3: window system (ported from Pi5) + USB mouse + `win`
61cb663  arm-rpi3: USB (DWC OTG) bring-up + HDMI keyboard shell
1837e74  arm-rpi3: enable HDMI framebuffer output
```

push 済み。診断用 kprintf は全て除去済み。

### 未コミットで温存しているもの (この作業と無関係)
- `apps/abcl_program.c` — AIPL の lock-free MPSC リングバッファ作業 (344+/305-)。**触らない**。
- `include/version.h` — ビルド自動生成 (毎ビルド変わる)。コミット不要。

## 今回の修正連鎖 (ethernet が動かなかった 3 つの真因) ★最重要

`davidxyz/xinuPi` (Pi1/Pi3 smsc9512, 動作実績) と比較 → **dwc は完全同一**、差は ethernet 設定。
動く smsc9512 は `HW_CFG_MEF|HW_CFG_BIR|HW_CFG_BCE` + 非ゼロ `BURST_CAP` を設定していた。

1. **`BURST_CAP=0` (飢餓の真因)** — `lan78xx.c`。MEF 有効 + BURST_CAP=0 だとデバイスが
   空 bulk-IN を連発 → RX 完了コールバックが無条件即再投入 → タイトループで CPU 飢餓
   → **ETH0 を開いた瞬間シェルが死ぬ**。
   修正: `HW_CFG_MEF` + `USB_CFG_BIR` + `BURST_CAP = DEFAULT_HS_BURST_CAP_SIZE/HS_PKT_SIZE (0x25)`。

2. **MAF レジスタオフセット誤り (ping の真因)** — `lan78xx.h`。`MAF_HI_0/LO_0` を
   `0x150/0x154` と書いていたが正しくは **`0x400/0x404`** (MAF_BASE=0x400)。MAF[0]=自分の MAC
   が一度も設定されず、RFE DA_PERFECT が自分宛 unicast に一切マッチせず **全 unicast ドロップ**
   (broadcast だけ通る)。→ ARP 応答/ICMP echo を受信できず ping 不成立。
   修正: `MAF_HI_0=0x400 / MAF_LO_0=0x404`。

3. **bulk defer 間隔の未定義動作 (RTT ~10s の真因)** — `usb_dwc_hcd.c` の `defer_xfer_thread`。
   NAK 再試行間隔を interrupt 用の周期公式 `(1 << (bInterval-1))/USB_UFRAMES_PER_MS` で計算。
   bulk は bInterval=0 → **`1 << -1` (UB)** → 巨大な defer → bulk-IN が数秒滞留 → RTT ~10-16s。
   修正: **`!usb_is_interrupt_request(req)` (bulk/control) は defer=1ms 固定**。interrupt は従来通り。

### その他の修正 (commit a020953)
- **固定 MAC `b8:27:eb:c0:ff:ee`** (`etherInit.c`) — `platform.serial_*`=0 だと `randomEthAddr`
  が `srand(clkcount())` で毎起動ランダム → peer の ARP キャッシュが無効化。rpi3 は固定値に。
- **ETH0 open を別スレッド化** (`main.c` `eth_open_all`, 64KB スタック) — `etherOpen` →
  `smsc9512_wait_device_attached` は timeout 無し wait なので、inline だと USB 列挙待ちで
  main (=シェル起動) がブロックする。8KB スタックだと lan78xx_open の深い chain で overflow。
- **no-promisc** — `RFE_CTL = BCAST_EN | DA_PERFECT` (UCAST_EN は外す)。UCAST_EN は全 unicast
  受信でコンソールを劣化させ、ping も直さない (問題は filter でなく MAF オフセットだった)。

## LAN78xx 確定構成 (動作する設定値)
- `RFE_CTL = BCAST_EN(0x400) | DA_PERFECT(0x2)` (no UCAST_EN)
- `HW_CFG |= MEF(0x10)`、`USB_CFG0 |= BIR(0x40)`
- `BURST_CAP = 0x25` (= 18944/512)、`BULK_IN_DLY = 0x800`
- `FCT_RX_FIFO_END=0x27, FCT_TX_FIFO_END=0x11`
- MAF[0] = 自分の MAC @ **0x400/0x404** + `MAF_HI_AF_EN=0x80000000`
- RX_ADDRL/H = 0x11C/0x118 (これは元から正しい)
- VID 0x0424 / PID 0x7800、bRequest 0xA0(write)/0xA1(read)、ID_REV=0x78000002
- RX header 10 byte (RX_CMD_A/B/C, len=RX_CMD_A&0x3FFF, FCS は MAC_RX FCS_STRIP で除去済)
- TX header: TX_CMD_A(len|FCS bit22) + TX_CMD_B、8 byte

## 実機テスト手順 & 落とし穴 ★

ビルド: `cd compile && make PLATFORM=arm-rpi3` → `xinu.boot` 生成。
焼く: USB/SD を Mac に挿す → `cp xinu.boot /Volumes/XINU/kernel.img && sync && diskutil eject`
→ Pi に戻して電源再投入 (ユーザに依頼)。

★**シリアル読み取り** (Python termios, B115200):
- macOS の USB-TTL は再接続で**デバイス名が変わる** (`-120`↔`-1120`)。
  **毎回 `ls /dev/cu.usbserial-*` で確認**。盲目デバッグで長時間ハマった主因。
- 送信は遅く char-by-char (~25-30ms/char) で送る (Pi UART RX ドロップ回避)。
- ★**`rxf#` 等の ISR からの出力が出ても「シェルが応答する」とは限らない**
  (ISR 出力はスレッド飢餓中でも出る)。応答確認は必ず `help` 等を送ってエコーを見る。
- インライン Python が前回分シリアルポートを掴むことがある → `pkill -f usbserial` で kill。

★**Mac↔Pi ネットワーク**: Pi はルータの LAN ポート、Mac は同ルータの WiFi (192.168.3.202)。
同一 /24。Pi に静的 IP: `netup ETH0 192.168.3.50 255.255.255.0 192.168.3.1`。
Mac から `ping 192.168.3.50`。Mac ファイアウォールは無効 (確認済)。
パケットレベル確認は Mac で `sudo tcpdump -ni en0 host 192.168.3.50` (要 sudo, `!` 前置で実行)。

## 既知の制約 / 次の候補作業

- `ps` / `help` コマンドはシリアルに出力が出ない (ping/netup は出る)。未調査の脇issue。
- `MAX_RX_REQUESTS=1` のまま (smsc9512 流用)。高スループット時は増やす余地あり。
- ICMP echo 応答は icmpDaemon (prio 30) 経由の非同期パス。負荷時の挙動は未評価。
- **TCP 実機疎通テスト済 (2026-05-27)**: ✅ **TCP 双方向データ転送は完全動作**。
  - Pi `nc -l 192.168.3.50 9999` リッスン → Mac `echo ... | nc 192.168.3.50 9999` → 文字列が
    Pi シリアルに着信 (Mac→Pi 生 TCP データ OK)。
  - `telnetserver` 起動 → Mac `nc 192.168.3.50 23`: **接続成立 (port 23) + telnet ネゴ +
    プロンプト `xsh$` 受信 + 入力エコー** が TCP 上で動作 (Pi→Mac データ OK)。
  - ⚠️ **残課題 = telnet リモートシェルが完全には使えない** (中核の TCP/TX/RX は正常):
    ★まず **TX-wedge 説は誤りと判明** — `smsc9512_tx_complete` に診断を入れて計測したところ
    **TX は GET→HAVE→DONE(st=0) で全て完了**(telnet コマンド中も #111-120 が揃う)、ping も
    2/2 0%loss ~10ms で安定。**TX/RX/生TCP は完全動作**。
    ★telnet シェル側の症状: ❶ コマンド**出力**(例 `help` の一覧)がクライアントにもシリアルにも
    出ない(プロンプト/入力エコーは届く) ❷ 接続が 1〜2 回でタイムアウトしだす(telnetServer の
    accept スレッド枯渇)。`telnetWrite/Flush/Server`・shell の fd 引継ぎ(`thrtab[child].fdesc[1]
    =stdout=telnetdev`, ready 前に設定)・`telnetRead` の EOF 伝播 — **コード検査では全て正しい**
    のに実機で出力が消える。盲目シリアル + 再起動ループ + サーバ枯渇で原因特定に至らず。
  - ★★**tcpdump で真因特定 (2026-05-27)**: 問題は telnet シェルでなく **TCP スタックの接続管理**。
    `sudo tcpdump -ni en0 -X 'tcp port 23 and host 192.168.3.50'` で観測:
    Pi(.50:23)→Mac の **SYN-ACK の seq が 2820 と極小・接続ごとに増加** → Mac が **RST** 連発、
    Pi は死んだ接続に SYN-ACK を出し続け **telnetServer がスピン** → 新規接続(別 port)の SYN に
    **一切 SYN-ACK を返さない**(=accept されず connect timeout)。
  - ★関係する定数 (`include/tcp.h`): **`TCP_SEQINCR=904`** (ISS を接続毎に +904 だけ増やす=小さく
    予測可能。`tcpSetup.c` の `tcpIss()` は `clktime`(秒) 起点 + 904)、**`TCP_TWOMSL=5000`** (TIME_WAIT
    わずか 5 秒)。**小さい ISS + 短い TIME_WAIT + 高速再接続** → seq/TIME_WAIT 衝突 → RST。
  - ★★**適用済み修正 (commit `3f5b6de`)**: ① `tcpSetup.c tcpIss()` を `clkticks<<10`+乱数base+
    接続毎bump で ISS を広く分散 (接続が安定・再接続枯渇が改善) ② `telnetWrite.c` の **osem リーク**
    (flush 失敗時 signal せず return → 以降全出力ブロック) を修正 ③ `telnetFlush.c` の早期 return が
    **restore(im) 漏れ** (割り込み無効のまま=フリーズ要因) を修正 ④ `telnetControl.c` FLUSH に osem
    排他追加。→ **シェルスレッドの出力 (起動バナー等) はクライアントに届くようになった**。
  - ⚠️ **未解決の残課題**: telnet で**子コマンドスレッドの出力** (`help` の一覧等) がクライアントに
    届くのが**間欠的** (実行ごとに変動: あるときバナーまで届き、あるときプロンプトのみ)。
    子スレッド fd は正しく telnetdev (=26、`[xsh_help: stdout=26]` で確認)。シェルスレッド出力は
    届くが子スレッド出力は届きにくい = **telnet デバイスと子スレッド実行のタイミング依存の並行性
    問題**。osem/flush/restore を直しても完全には解消せず。盲目シリアル+再起動ループでは収束せず。
  - ★★**TELNET_TRACE で判明 (2026-05-27)**: `telnet.h` の `TRACE_TELNET` を有効化 (kprintf ベースに
    改善済み) し telnetFlush にトレースを入れて telnet help を観測:
    - `[tn t24] flush ostart=0 state=2 phw=19` が 200ms 毎に連続 = telnetServer(tid24)の flush ループ。
      **ostart が常に 0** = **子スレッド(help)の出力が telnetWrite に一切バッファされていない**
      (子スレッドからの flush も皆無)。→ **子の printf が telnetWrite/device 26 に到達していない**
      ことを確定 (子の fdesc[1]=26=telnetdev は `[xsh_help: stdout=26]` で確認済みなのに)。
    - `[tn t30] Read error` + 以前のエコー破損 ("helpp"/"hellp") = **telnet 入力経路(telnetRead)も
      バグ**。help が破損入力/誤 nargs で別パスに入り出力しない可能性。
  - ★結論: telnet 疑似デバイスは**入力・出力の両経路にバグ**。断片修正でなく **telnet (telnetRead/
    telnetWrite/telnetServer の osem/buffer 所有権 + shell の child fd/printf 経路) の包括的見直し**が要る。
    `route/memstat/ps` のシリアル無出力は別件 (`help` はシリアルでは出力する)。
  - ★★**telnetWrite 入口 tid トレースで確定 (2026-05-27)**: telnet help 中に telnetWrite を呼ぶのは
    **シェルスレッド(tid32)のみ** (内容は prompt/echo の `.xhelp..help..x..x` だけ。help の
    コマンド一覧は皆無)。**help の子スレッド(tid33+)からの telnetWrite 呼び出しは一件も無い**。
    → **help 子スレッドの printf がそもそも telnetWrite に到達していない** (telnetWrite より上流の
    問題)。子の fdesc[1]=26=telnetdev は確認済みなのに、シェル(32)の fputc(26) は telnetWrite に
    届き子の fputc(26) は届かない = printf→fputc→putc→telnetPutc 経路にスレッド固有の何かがある、
    または help 子がコマンド一覧 printf に到達していない (破損入力で別パス/早期return)。
    (`[tn t32] Read error` も観測 — 接続クローズ由来の可能性)。
  - ★次の手: `xsh_help` に「コマンド一覧 printf 到達 + その時の gettid()/stdout」トレースを足し
    (a) help 子が printf を実行するか (b) fputc が dev26 へ向かうか を確定。さらに `fputc/putc`
    (lib/libxc) で子スレッドの書き込みが telnetPutc に届くか追う。または telnet 実装の包括見直し。
  - ★TELNET_TRACE の使い方: `include/telnet.h` の `//#define TRACE_TELNET 1` を有効化 (kprintf で
    シリアルに `[tn tN] ...`)。
- 関連プロジェクト: `xinu-rpi5` (Pi5 AArch64, 別repo) は窓システムの移植元。

## 主要ファイル
- `device/smsc9512/lan78xx.c` / `lan78xx.h` — LAN78xx ドライバ本体
- `device/smsc9512/etherInit.c / etherOpen.c / etherInterrupt.c` — chiptype で smsc9512/LAN78xx 分岐
- `device/smsc9512/etherInterrupt.c` `smsc9512_rx_complete` / `lan78xx_rx_complete_parse` — RX
- `system/platforms/arm-rpi3/usb_dwc_hcd.c` `defer_xfer_thread` — NAK 再試行 (RTT 修正箇所)
- `system/main.c` `eth_open_all` + spawn — ETH0 非同期 open
- `compile/platforms/arm-rpi3/xinu.conf` — デバイス構成
- メモリ: `~/.claude/.../memory/project_xinu_rpi3_port.md` に全経緯
