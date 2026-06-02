# Xinu Pi3 デスクトップ ユーザーガイド

Raspberry Pi 3 B+ 上のベアメタル Embedded Xinu（ビルド `xinu-gwm-b69`）の
使い方ガイドです。HDMI デスクトップ（gwm）、WiFi、対話シェル、3D グラフィックス、
GC 連動の分散 N-Queens、Mac ブラウザ連携の操作方法をまとめます。

---

## 1. 起動（デプロイ）

カーネルを書き換えたら、SD 経由で起動します。

1. Pi3 の**電源を完全に切る**（電源ケーブルを抜く）
2. SD カードを抜いて Mac のカードリーダーに挿す（`/Volumes/XINU` が現れる）
3. `compile/sdcard-rpi3/kernel.img`（= ビルドした `xinu.boot`）を SD の `kernel.img` に上書きコピー
4. 取り出して Pi3 に戻し、電源 ON

> 起動には ~40 秒かかることがあります。HDMI にデスクトップ、有線 LAN 経由で
> `http://192.168.3.50:8080/` が応答すれば起動完了です。

---

## 2. HDMI 画面（gwm デスクトップ）

電源 ON で、HDMI に以下のウィンドウが表示されます（1280×800）。

| 位置 | ウィンドウ | 内容 |
|---|---|---|
| 左上 | **Xinu Pi3** | ボード情報 |
| 左中 | **AIPL console (print)** | AIPL の print 出力 |
| 左下 | **Soft keyboard** | ソフトキーボード表示 |
| 中央 | **AIPL actors (live)** | アクター一覧（ID/クラス/状態/メッセージ数） |
| 中央下 | **graphics** | 3D ワイングラス（`wine` コマンドで回転） |
| 右 | **Shell** | 本物の xsh（コマンド実行・出力） |
| 右下 | **WiFi マーク** | 白=未接続 / 緑=接続中（下に接続中の SSID） |

> HDMI 側はマウスでの操作はできません。キーボード入力・WiFi の AP 選択は
> 次の Mac ブラウザページから行います。

---

## 3. Mac ブラウザページ（/pi3）

Mac でダッシュボードを起動し、ブラウザで操作します。

```sh
cd /Users/kodamay/ocaml-app/abclcp-project
python3 src/python-aipl/aipl_main.py --dashboard 8899 \
    aice-pi-evolution/experiments/2026-05-27_dining_mac_xinu/local_diners.abcl
```

ブラウザで **http://127.0.0.1:8899/pi3** を開きます。

- 画面は Xinu HDMI と同じ窓配置のミラーです。
- **Shell 窓をクリックしてフォーカス**（黄枠）すると、キーボード入力が Pi3 の
  xsh に送られます。Shell 窓は実機の出力をそのまま映します。
- **右下の WiFi マーク**をクリックすると AP 一覧が出ます（後述）。

---

## 4. シェル（xsh）の使い方

Mac の /pi3 ページで Shell 窓をフォーカスし、コマンドを打って Enter。

```
xsh$ ls           # ファイル一覧
xsh$ ps           # スレッド一覧
xsh$ help         # コマンド一覧
xsh$ memstat      # 空きメモリ
```

> `edit` のような全画面エディタは、行ベースの窓では正しく描画できません。
> 行単位のコマンド（ls / ps / cat / cc など）は動作します。

---

## 5. WiFi の操作

### 5.1 シェルコマンド（推奨）

```
xsh$ wifi status      # 現在の接続状況（SSID + IP / GW）
xsh$ wifi on          # スキャン → 番号で AP 選択 → パスワード入力 → 接続
xsh$ wifi off         # 切断
```

`wifi on` の流れ：

```
Scanning for access points (~30s; the radio drops briefly)...
Available networks:
  1: 0C73298EF82D-5G
  2: Buffalo-G-56A2
  ...
Select AP number (0 = cancel): 1
Password for "0C73298EF82D-5G" (blank = open): ********
Connecting...
WiFi: connected to "0C73298EF82D-5G"  ip 192.168.3.52 ...
```

### 5.2 ブラウザの WiFi マーク

/pi3 ページ右下の WiFi マーク（白=未接続）をクリック → AP 一覧 → 選択 →
パスワード入力で接続。接続すると緑に変わります。

> スキャンは一瞬 WiFi を切ります（無線が全チャンネルを再同調するため）。
> 選んで接続し直せば復帰します。

### 5.3 「接続済みなのに ping が通らない」とき

```
xsh$ wifi-invest
```

応答スレッドの再起動・gratuitous ARP 送信・ゲートウェイへの ping を、
**通るまで自動でリトライ**します（最大 ~20 秒で必ず終了）。

- 「data path is UP」→ Pi3 側は正常。Mac から ping を再試行してください。
- 「still no gateway」→ チップのデータ経路が停止。`wifi off`→`wifi on`、
  または電源再投入。

> WiFi は**再起動後に自動接続しません**。起動のたびに `wifi on` してください。

---

## 6. 3D グラフィックス（wine）

```
xsh$ wine
```

graphics 窓の中で、ワイヤフレームの 3D ワイングラスが X/Y/Z 軸まわりに
30 回転して停止します（`spin N/30` を表示）。

> 連続回転は単一スレッドの HTTP/WiFi 応答を圧迫するため、**実行時のみ**回す
> 設計です。

---

## 7. GC 連動の分散 N-Queens ベンチ

### 7.1 起動とベンチ実行

```sh
H=192.168.3.50:8080
# 負荷分散（Dispatcher + Worker×4）と GC（Collector）を起動
curl -s "http://$H/api/loadbal/init"
curl -s "http://$H/api/gc-actor/init"
curl -s "http://$H/api/gc-actor/enable?on=1"

# N-Queens を投入（第1列ごとに分散）
curl -s "http://$H/api/loadbal/nqueens?n=8"

# 集計結果を 1 回ポーリングで取得（push 集約）
curl -s "http://$H/api/loadbal/nqueens-result"
#   -> nqueens-result done=1 received=8 expected=8 total=92
```

- `n=8` → 92、`n=9` → 352（既知の正解と一致）。
- 結果は Worker → Dispatcher への push で集約されるため、タスクを個別に
  ポーリングする必要はありません。

### 7.2 GC 統計

```sh
curl -s "http://$H/api/gc-actor/stats"
#   gc-actor obj=6 enabled=1 period_ms=5000 threshold_ms=30000
#   sweep_count=.. swept_total=.. last_scanned=..
```

`swept_total=0` は「回収対象なし（全アクター生存）」を意味し、常駐アクターのみ
のときの正常状態です。

詳細な計測レポートは `docs/nqueens_bench_report.pdf` を参照してください。

---

## 8. トラブルシューティング

| 症状 | 対処 |
|---|---|
| HTTP が応答しない（000） | 重い処理（scan/大きな N-Queens）の最中。単一スレッドのため一時的に停滞。数十秒待つと回復 |
| 接続済みなのに ping 不通 | `wifi-invest`。直らなければ `wifi off`→`wifi on` か電源再投入 |
| 起動後 WiFi が切れている | 仕様（自動再接続しない）。`wifi on` で再接続 |
| Shell 窓に出力が出ない | `printf` 経路の不具合（過去事例）。`lib/libxc` を再ビルド（`rm -f lib/libxc/*.o lib/libxc.a && make`） |
| 画面が固まる | 連続アニメ等の負荷。`wine` は実行時のみ回す設計。電源再投入で回復 |

---

英語の概要は `README.md` を参照してください。
