# NEXT_SESSION — xinu-raz / arm-rpi3 (Raspberry Pi 3 B+ 実機移植)

最終更新: 2026-05-27 / ブランチ `arm-rpi3-port` / repo `github.com/yaskodama/xinu-rpi`

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
- TCP/telnet スタックは存在するが実機での疎通は未テスト (UDP/ICMP まで確認済)。
- 関連プロジェクト: `xinu-rpi5` (Pi5 AArch64, 別repo) は窓システムの移植元。

## 主要ファイル
- `device/smsc9512/lan78xx.c` / `lan78xx.h` — LAN78xx ドライバ本体
- `device/smsc9512/etherInit.c / etherOpen.c / etherInterrupt.c` — chiptype で smsc9512/LAN78xx 分岐
- `device/smsc9512/etherInterrupt.c` `smsc9512_rx_complete` / `lan78xx_rx_complete_parse` — RX
- `system/platforms/arm-rpi3/usb_dwc_hcd.c` `defer_xfer_thread` — NAK 再試行 (RTT 修正箇所)
- `system/main.c` `eth_open_all` + spawn — ETH0 非同期 open
- `compile/platforms/arm-rpi3/xinu.conf` — デバイス構成
- メモリ: `~/.claude/.../memory/project_xinu_rpi3_port.md` に全経緯
