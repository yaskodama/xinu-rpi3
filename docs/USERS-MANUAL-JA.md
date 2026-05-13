# Xinu (RAZ 拡張版) ユーザズマニュアル

本マニュアルは、Embedded Xinu をベースに

- 階層型ファイルシステム **XFS** (RAM ディスク上)
- 組込みのちっちゃな C コンパイラ **cc** とバイトコード VM **a.out**
- アクタ指向小型言語 **ABCL/c+** とその C トランスレータ **abclc**
- ウィンドウマネージャ **wm** (ARM/QEMU 上のフレームバッファ GUI)
- Unix 風シェル拡張 (`ls`/`cd`/`cat`/`make`/`edit` など)

を組み込んだ拡張版 Xinu の使い方をまとめたものです。本家 Embedded Xinu の機能 (スレッド・セマフォ・ネットワーク等) はそのまま使えます。

---

## 目次

1. [はじめに](#1-はじめに)
2. [起動方法](#2-起動方法)
3. [シェルの使い方](#3-シェルの使い方)
4. [XFS — ファイルシステム](#4-xfs--ファイルシステム)
5. [cc / run — C コンパイラとランチャ](#5-cc--run--c-コンパイラとランチャ)
6. [abclc — ABCL/c+ アクタ言語](#6-abclc--abclc-アクタ言語)
7. [make — 簡易ビルドシステム](#7-make--簡易ビルドシステム)
8. [edit — 組込みエディタ](#8-edit--組込みエディタ)
9. [wm — ウィンドウマネージャ](#9-wm--ウィンドウマネージャ)
10. [サンプルプログラム詳説](#10-サンプルプログラム詳説)
11. [開発ワークフロー](#11-開発ワークフロー)
12. [トラブルシューティング](#12-トラブルシューティング)

---

## 1. はじめに

Xinu は Douglas Comer が教育用に開発した小型 UNIX 風 OS です。本拡張版はこれに「OS 上で C/ABCL ソースを編集・コンパイル・実行できる自己完結したワークベンチ」を載せたもので、ホスト側のクロスコンパイラに頼らずとも、QEMU の中だけでプログラム開発のループを完結できます。

対象プラットフォーム:

| プラットフォーム名 | 用途 | 特徴 |
| --- | --- | --- |
| `arm-qemu` | QEMU `-M versatilepb` | UART シェル + フレームバッファ + WM 自動起動 |
| `arm-rpi`  | Raspberry Pi 実機 | UART/HDMI/USB キーボード |

XFS / cc / abclc / make / edit はどのプラットフォームでも同じように使えます。WM 関連 (`rotlines`, `wm_line` など) は arm-qemu でのみ有効です。

---

## 2. 起動方法

### 2.1 ビルド

```sh
cd compile
make PLATFORM=arm-qemu        # 既定。arm-rpi も可
```

成功すると `compile/xinu.boot` が生成されます。

### 2.2 QEMU で実行

ホストが macOS/Linux で `qemu-system-arm` がインストール済みであれば、用意してあるラッパスクリプトが便利です。

#### コンソール専用モード (フレームバッファなし)

```sh
./compile/run-console.sh
```

- UART シェル (`xsh$`) が端末に多重化されます。
- 終了は **Ctrl-A → x**。
- 環境変数: `XINU_MEM` (既定 `128M`)、`XINU_PLATFORM` (既定 `arm-qemu`)。

#### ウィンドウモード (WM デモ込み)

```sh
./compile/run-window.sh
```

- LCD フレームバッファが macOS の Cocoa ウィンドウに表示されます。
- UART シェルは起動した端末で動きます。`-serial mon:stdio` で QEMU モニタも同居。
- 終了はウィンドウを閉じるか、端末で **Ctrl-A → x**。
- ウィンドウは角をドラッグで最大 4 倍まで滑らかに拡大可能 (zoom-to-fit)。

### 2.3 起動シーケンス

`system/main.c` の `main()` スレッドが以下を順に行います。

1. OS バージョン・メモリレイアウトを表示
2. `xfsBootstrap()` で RAMDISK0 を XFS でフォーマット & `/` にマウント
3. `/home/hello.c`, `/home/sum.c`, `/home/abclcp/abclc/PingPong.abcl`, `/home/abclcp/abclc/RotLines.abcl` をシード書き込み
4. ネットワークデバイスを open (`NETHER` 有効時)
5. CONSOLE/TTY1 を open
6. arm-qemu のときだけ `wm_main` スレッドを `create()`
7. シェルスレッドを CONSOLE/TTY1 ごとに起動

ブート完了後、シェルプロンプトに

```
xsh$
```

と表示されればコマンド入力可能です。

---

## 3. シェルの使い方

### 3.1 基本

```
コマンド名 [引数...]    [< 入力リダイレクト] [> 出力リダイレクト] [&]
```

- `&` でバックグラウンド起動
- `<` `>` で入出力デバイスへリダイレクト
- 補完・履歴はありません

### 3.2 コマンド一覧 (本拡張版で追加・拡張されたもの)

| コマンド | 使い方 | 説明 |
| --- | --- | --- |
| `pwd` | `pwd` | カレントディレクトリ表示 |
| `cd` | `cd [PATH]` | ディレクトリ変更 (省略時は `/`) |
| `ls` | `ls [-l] [PATH]` | ディレクトリ一覧。`-l` で type/size 表示 |
| `cat` | `cat FILE...` | ファイル内容表示 |
| `mkdir` | `mkdir DIR...` | ディレクトリ作成 |
| `rmdir` | `rmdir DIR...` | 空ディレクトリ削除 |
| `touch` | `touch FILE...` | 空ファイル作成 / mtime 更新 |
| `rm` | `rm FILE...` | ファイル削除 |
| `cp` | `cp SRC DST` | コピー (DST は truncate) |
| `mv` | `mv SRC DST` | リネーム/移動 |
| `write` | `write FILE TEXT...` | 引数を空白区切りで連結し改行付きで FILE に書き込み |
| `edit` | `edit FILE` | 組込み Emacs 風エディタ |
| `mkfs` | `mkfs DEVICE [VOL]` | ブロックデバイスを XFS でフォーマット |
| `mount` | `mount DEV MNT` | XFS をマウント |
| `umount` | `umount MNT` | XFS をアンマウント |
| `cc` | `cc SRC [-o OUT]` | C ソースを a.out にコンパイル |
| `abclc` | `abclc SRC.abcl [-o OUT]` | ABCL → C → a.out まで一気通貫 |
| `make` | `make [TARGET...]` | カレントディレクトリの Makefile を実行 |
| `run` | `run PATH` | a.out を実行 |
| `rotlines` | `rotlines [FRAMES] [DEG/F]` | WM 上に回転線を描く |
| `clear` | `clear` | 画面クリア |
| `sleep` | `sleep N` | N ms スリープ |

#### 暗黙の `run`

シェルが知らないコマンド名を打つと、シェルは最後に `aoutRun(<その名前>)` を試します (`shell/shell.c:332` 付近)。つまり `cc hello.c -o hello` のあとは

```
xsh$ hello
hello...
```

のようにファイルパス相当の名前で直接プログラムを起動できます。

---

## 4. XFS — ファイルシステム

### 4.1 概要

`include/xfs.h` で定義される 4 KB ブロックの階層型 FS で、起動時に 16 MB の RAM ディスク (`RAMDISK0`) 上に作られ `/` にマウントされます。

| 項目 | 値 |
| --- | --- |
| ブロックサイズ | 4096 B |
| 最大ファイル名長 | 56 B |
| inode サイズ | 128 B |
| 直接ブロック | 12 (= 48 KB) |
| 一段間接 | 1024 ブロック (≒ 4 MB) |
| 二段間接 | あり |
| マジック | `XFS!` (0x58465321) |

ディレクトリは inode の中身が `struct xdirent[]` (1 エントリ 64 B) で表現され、`xfsReaddir()` で順次読み出します。

### 4.2 スレッドローカル CWD

`xfsChdir()`/`xfsGetcwd()` はスレッドごとの絶対パス CWD を保持しています。シェル A で `cd /home` してもシェル B には影響しません。

### 4.3 ブートストラップ

`xfsBootstrap()` は以下を行います。

1. `RAMDISK0` の先頭 64 KB に XFS マジックがあるかを調べ、なければ `xfsMkfs("RAMDISK0", "xfs")` で初期化
2. `/` に `RAMDISK0` をマウント
3. ルート inode を準備

### 4.4 主な API (C プログラムから)

```c
int fd = xfsOpen("/home/foo", XFS_O_RDWR | XFS_O_CREAT | XFS_O_TRUNC);
xfsWrite(fd, buf, n);
xfsRead (fd, buf, n);
xfsSeek (fd, 0, XFS_SEEK_SET);
xfsClose(fd);

xfsMkdir("/home/sub");
xfsRename("/home/old", "/home/new");
xfsUnlink("/home/foo");
```

### 4.5 RAM ディスクの永続性

RAMDISK0 は名前のとおり RAM です。**QEMU を終了するとファイルは消えます**。永続的に残したいソースはホストでバックアップしてください (例: コピー&ペースト、`tftp`、ホスト側にビルド前の素材を置いておく等)。

---

## 5. cc / run — C コンパイラとランチャ

### 5.1 サポートする C のサブセット

`system/cc.c` 冒頭に明記されています。

- 1 ソースファイル, `int main(void)` 1 個 + 引数なしの補助関数
- 宣言: `int x;`, `int x = expr;` のみ (`char` 等は型キーワードを認識するだけ)
- 文: `if/else`, `while`, `for`, `return`, ブロック, 式文
- 式: 整数・文字列・識別子・`+ - * / %`、`== != < <= > >=`、`! && ||`、単項 `-`、括弧
- 組込み関数のみ呼び出し可
- `#include <...>` / `"..."` は受理して無視

未サポート: 構造体・ポインタ・配列・浮動小数・複数ファイル。

### 5.2 組込み関数 (Built-in)

`include/aout.h` で番号が定義されています。

| BI | 関数 | 用途 |
| --- | --- | --- |
| 0 | `printf(fmt, ...)` | 文字列・整数フォーマット |
| 1 | `puts(s)` | 文字列+改行 |
| 2 | `putchar(c)` | 1 文字出力 |
| 3 | `getchar()` | 1 文字入力 |
| 4 | `exit(code)` | 終了 |
| 5 | `rgb(r,g,b)` | BGR565 16bit カラー値 |
| 6 | `wm_line(idx,x1,y1,x2,y2,color)` | WM のユーザ線スロット (0..7) に登録 |
| 7 | `wm_render(on)` | ユーザ線レンダの有効/無効 |
| 8 | `wm_clear()` | ユーザ線クリア |
| 9 | `sleep_ms(ms)` | スリープ |
| 10/11 | `isin(deg)`/`icos(deg)` | Q12 固定小数 (4096 = 1.0) |
| 12/13 | `screen_w()`/`screen_h()` | 画面サイズ取得 |

### 5.3 a.out 形式

`include/aout.h` の `struct aout_header` で記述。

```
+--------------------------+
| magic = "XAOU"           |
| version = 1              |
| code_size                |
| const_size               |
| entry                    |
| nlocals (main の局所数)   |
+--------------------------+
| バイトコード本体           |
+--------------------------+
| 文字列定数プール (零終端)  |
+--------------------------+
```

VM はスタック型 (256 entries) で、`OP_PUSH_I32` `OP_LOAD_LOC` `OP_ADD` `OP_JZ` `OP_CALL_BI` などの 1 バイトオペコードを持ちます。詳しくは `system/aout.c` の `aoutRun()`。

### 5.4 シェルでの使い方

```
xsh$ cc /home/hello.c -o /home/hello
xsh$ run /home/hello
hello...
xsh$ /home/hello              # 暗黙 run でも OK
hello...
```

---

## 6. abclc — ABCL/c+ アクタ言語

### 6.1 言語仕様 (本実装の範囲)

ABCL/c+ は「クラス + メッセージ送信」によるアクタ指向の小型言語で、本実装では **abclc が C へ変換し、その C を cc がバイトコード化** する 2 段階構成です。

```
class ClassName {
    var field1;
    var field2;

    method methodName(p1, p2) {
        // 文: 代入, if/else, while, printf, send
        field1 = p1 + 1;
        send other.methodName(arg1, arg2);
    }
}

main {
    new ClassName a;
    new ClassName b;
    send a.methodName(b, 10);
}
```

- 暗黙の識別子: `self` (現在のアクタ ID)、`sender` (送信元 ID)
- 1 メソッドあたり `send` は最大 1 回 (末尾呼び出し型)。これにより 1 アクタは 1 メッセージずつ逐次的に処理されます。
- 値はすべて `int` 相当
- 制限: 4 クラス, 1 クラスあたり 16 メソッド, 8 フィールド, 8 引数 など (生成 C ランタイム側の上限)

### 6.2 シェルでの使い方

```
xsh$ cd /home/abclcp/abclc
xsh$ abclc PingPong.abcl
abclc: translated PingPong.abcl -> PingPong.c
abclc: compiled PingPong.c -> PingPong
xsh$ run PingPong
```

`-o NAME` を付けると、`NAME.c` (中間 C) と `NAME` (a.out) の両方がそのパスに作られます。

### 6.3 ランタイム

abclc が出力する C は、cc の組込み関数 (`printf` 等) と少数のスケジューリング用ヘルパだけを使います。1 メッセージ = 1 タイムスライスとして実行され、`send` 後はメッセージキューにぶら下がります。

WM 連動の ABCL ビルトイン (`wm_line`, `isin`, `icos`, `sleep_ms`, `rgb` 等) は cc 側のビルトインと同名で、ABCL から直接呼べます (RotLines.abcl 参照)。

---

## 7. make — 簡易ビルドシステム

`shell/xsh_make.c` の挙動:

1. CWD に `Makefile` がなければ、`*.c` と `*.abcl` を走査して自動生成
2. `Makefile` を読み、`TARGETS = ...` で指定された (またはルール宣言順の) 各ターゲットをビルド
3. `.c` → `cc SRC -o TARGET`、`.abcl` → `abclc SRC -o TARGET`

### 7.1 Makefile 形式

```
# コメント
TARGETS = hello sum PingPong RotLines

hello:    hello.c
sum:      sum.c
PingPong: PingPong.abcl
RotLines: RotLines.abcl
```

明示ルールが無いターゲットは、`TARGET.c` → `TARGET.abcl` の順に存在チェックして自動推論します。

### 7.2 使用例

```
xsh$ cd /home
xsh$ ls
hello.c
sum.c
xsh$ make
make: generated Makefile (2 .c, 0 .abcl)
    cc hello.c -o hello
    cc sum.c -o sum
make: built 2 target(s)
xsh$ ./hello
hello...
```

---

## 8. edit — 組込みエディタ

`shell/xsh_edit.c` の Emacs 風モードレスエディタ。最大 400 行 × 256 文字。

### 8.1 起動

```
xsh$ edit /home/hello.c
```

存在しないパスを指定すると新規バッファで開きます。

### 8.2 キーバインド

| キー | 動作 |
| --- | --- |
| `C-f` / `C-b` | 1 文字 進む / 戻る |
| `C-n` / `C-p` | 1 行 下 / 上 |
| `C-a` / `C-e` | 行頭 / 行末 |
| `←↑→↓` | 矢印 (ESC[ シーケンス) |
| `C-d` | カーソル位置を削除 |
| `Backspace` | 1 文字戻して削除 |
| `Enter` | 改行挿入 |
| `Tab` | 空白 2 個 |
| `C-k` | 行末まで kill |
| `C-l` | 再描画 |
| `C-x C-s` | 保存 |
| `C-x C-c` | 保存して終了 |
| `C-g` | 保存せず終了 |

ステータス行に `L行:C列  status` が表示されます。

---

## 9. wm — ウィンドウマネージャ

### 9.1 概要

`apps/wm.c` は arm-qemu の VersatilePB 上で

- PL110 LCD コントローラ (`0x10120000`, 16bpp BGR565)
- PL050 PS/2 マウス (`0x10007000`)

を直接叩いて、1024×1024 仮想画面 (実画面 640×480) のデスクトップ風 GUI を表示します。`run-window.sh` で起動するとブート時に自動で `wm_main` が立ち上がります。

提供するもの:

- ステータスバー
- ドラッグ可能なデモウィンドウ 3 枚
- マウスカーソル
- ユーザ用描画フック `wm_set_user_render(void(*)(void))`
- 直接描画 API: `put_pixel_pub` / `fill_rect_pub` / `rect_outline_pub` / `draw_string_pub` / `wm_draw_line`

### 9.2 ユーザプログラムからの利用

C プログラム / ABCL からは a.out のビルトイン経由で

- `rgb(r, g, b)` で色作成
- `wm_render(1)` で線レイヤ ON
- `wm_line(idx, x1, y1, x2, y2, color)` で 0..7 番スロットの線を更新
- `wm_clear()` でクリア
- `screen_w()` / `screen_h()`

を使うのが基本パターンです (RotLines.abcl 参照)。

シェルコマンド `rotlines` はもう少し低レベルで、`wm_set_user_render(rot_render)` を使って毎フレームの描画関数を直接 WM に登録します。

---

## 10. サンプルプログラム詳説

ブート時に `system/main.c` が以下 4 本を XFS にシードします。

| パス | 種類 | 主役 |
| --- | --- | --- |
| `/home/hello.c` | C | `printf` |
| `/home/sum.c` | C | `for` ループ |
| `/home/abclcp/abclc/PingPong.abcl` | ABCL | アクタ間メッセージ |
| `/home/abclcp/abclc/RotLines.abcl` | ABCL + WM | アニメーション |

加えてリポジトリ内には次のソースがあり、コードリーディング教材として有用です。

| ファイル | 内容 |
| --- | --- |
| `apps/abcl_program.c` | abclc が **生成する側の C** の見本 (有限バッファ問題) |
| `apps/abcl_xinu_gui.c` | ABCL ↔ WM 連携ランタイム (有限バッファ・哲学者問題の描画) |
| `apps/wm.c` | WM 本体 |
| `shell/xsh_rotlines.c` | C で書かれた WM デモ |

以下、それぞれを詳しく見ていきます。

### 10.1 hello.c — 最初の一歩

シードされる内容 (`system/main.c:40-45`):

```c
#include <stdio.h>
int main(void) {
    printf("hello...\n");
    return 0;
}
```

ポイント:

- `#include` は cc では「受理して無視」。`stdio.h` を実際に開いたりはしません。
- `printf` はビルトイン番号 0 として呼ばれ、内部では `kprintf` 相当に転送されます。
- 戻り値 0 は `OP_RET` で `aoutRun()` の戻り値となり、シェルの終了コードに反映されます。

実行手順:

```
xsh$ cd /home
xsh$ cc hello.c -o hello
xsh$ run hello
hello...
xsh$ ./hello              # 暗黙 run
hello...
```

**a.out バイトコードの動き** (簡略):

```
ENTER 0                         ; main は局所なし
PUSH_STR offset_of("hello...\n")
CALL_BI 0, 1                    ; printf, 引数 1
PUSH_I32 0
RET
```

### 10.2 sum.c — 制御構造の例

```c
#include <stdio.h>
int main(void) {
    int i;
    int s;
    s = 0;
    for (i = 1; i <= 10; i = i + 1) {
        s = s + i;
    }
    printf("sum 1..10 = %d\n", s);
    return 0;
}
```

学べる点:

- 局所変数 `i`, `s` はそれぞれ `OP_LOAD_LOC` / `OP_STORE_LOC` で番号スロットにアクセスされる
- `for (init; cond; step)` は cc 内部で「init; while(cond) { body; step; }」に展開
- `+= ++ --` は未サポートなので `i = i + 1` のように書く必要がある
- フォーマット `%d` は `printf` ビルトイン側で解釈。`%s` も使える

実行例:

```
xsh$ cc /home/sum.c -o /home/sum
xsh$ run /home/sum
sum 1..10 = 55
```

### 10.3 PingPong.abcl — アクタ間メッセージング

ソース (`system/main.c:76-94` がシード):

```abcl
class Player {
    var hits;
    method bounce(other, n) {
        if (n > 0) {
            printf("Player %d: tick (n=%d) hits=%d\n", self, n, hits);
            hits = hits + 1;
            send other.bounce(self, n - 1);
        }
    }
}

main {
    new Player p1;
    new Player p2;
    send p1.bounce(p2, 6);
}
```

意味:

- `Player` クラスはフィールド `hits` (= 自分が叩いた回数)、メソッド `bounce(other, n)` を 1 つ持つ。
- `bounce` 本体: `n` が正なら、自分の状態を 1 進めて、相手のアクタに「自分を引数に、`n-1` で `bounce`」を送信。
- `main`: `p1`, `p2` を生成して `p1` に最初の `bounce(p2, 6)` を送る。これで p1 → p2 → p1 → ... が 6 回往復する。

実行手順:

```
xsh$ cd /home/abclcp/abclc
xsh$ abclc PingPong.abcl
abclc: translated PingPong.abcl -> PingPong.c
abclc: compiled PingPong.c -> PingPong
xsh$ run PingPong
Player 0: tick (n=6) hits=0
Player 1: tick (n=5) hits=0
Player 0: tick (n=4) hits=1
Player 1: tick (n=3) hits=1
Player 0: tick (n=2) hits=2
Player 1: tick (n=1) hits=2
```

注目:

- `self` はメソッド内の現在アクタ ID (生成順 0,1,...)
- `send` は **非同期**。呼んだ側のメソッドはそのまま終わり、メッセージはキューに入って次のスケジューリングで実行される
- `n - 1` が 0 で `if` を抜けると `send` が出ないので連鎖は自然停止

中間生成物 `PingPong.c` を読むと、abclc がどのように
「クラス・メソッド × アクタ」のディスパッチを `switch` 文に降ろしているかが分かります。

### 10.4 RotLines.abcl — ABCL + WM のアニメーション

`system/main.c:106-143` がシード:

```abcl
class Rotator {
    var angle;
    method spin(n) {
        if (n > 0) {
            wm_line(0, 512, 384,
                    512 + icos(angle) * 320 / 4096,
                    384 + isin(angle) * 320 / 4096,
                    rgb(255, 80, 80));
            wm_line(1, 512, 384,
                    512 + icos(angle + 90) * 320 / 4096,
                    384 + isin(angle + 90) * 320 / 4096,
                    rgb(80, 255, 80));
            // (180°, 270° も同様)
            sleep_ms(16);
            angle = angle + 3;
            send self.spin(n - 1);
        } else {
            wm_render(0);
        }
    }
    method start(n) {
        wm_render(1);
        send self.spin(n);
    }
}
main {
    new Rotator r;
    send r.start(360);
}
```

ポイント:

- `wm_render(1)` でユーザ線レイヤを ON にしてから、毎フレーム 4 本の線スロット (0..3) を更新。
- `icos`/`isin` は **Q12 固定小数** を返すので、半径 320 px をかけたら `4096` で割って実座標へ。
- `sleep_ms(16)` でだいたい 60 fps。
- 最後に `send self.spin(n-1)`。アクタ自身に再帰的に投げることでアニメーションループを実現。終了条件 (`n == 0`) で `wm_render(0)` を呼んで線レイヤを消す。

実行手順 (ウィンドウモード時):

```
xsh$ cd /home/abclcp/abclc
xsh$ abclc RotLines.abcl
xsh$ run RotLines       # QEMU ウィンドウに 4 本の線が回転表示される
```

### 10.5 rotlines (シェルコマンド) — C 直書き版の同等品

`shell/xsh_rotlines.c`。ABCL を経由せず、シェルコマンドが直接 WM に描画フックを登録する例です。

```
xsh$ rotlines            # 240 frames (~4 秒)
xsh$ rotlines 600 5      # 600 frames、1 frame で 5° 回す
```

仕掛け (`shell/xsh_rotlines.c:59-81`):

- `rot_render()` を `wm_set_user_render()` で WM スレッドに渡す
- WM は毎フレーム描画関数を呼ぶ。`rot_render` は最新の `rot_angle` から 4 本の端点を計算して `wm_draw_line` で描画
- メインスレッドは `for` ループで角度を進めながら `sleep(16)` を繰り返すだけ

ABCL 版との違い:

| 項目 | rotlines (C) | RotLines.abcl |
| --- | --- | --- |
| 描画方法 | `wm_draw_line` (毎フレーム関数) | `wm_line` (スロット型ステート) |
| 角度更新 | C のループ | アクタの再帰送信 |
| 用途 | WM 連動の最小例 | ABCL のアニメーション学習 |

### 10.6 apps/abcl_program.c — 有限バッファ問題 (生成 C の見本)

このファイルは「abclc が ABCL から生成する C のサンプル」です。実際の構成は次のとおり (`apps/abcl_program.c`):

- **クラス**: `Buffer`, `Producer`, `Consumer`, `Controller`
- 6 つの actor (`P0..P2`, `C0..C2`) と 1 つの `Buffer`、それらを束ねる `Controller`
- 各アクタは個別のメールボックス + Xinu スレッドを持ち、`abcl_actor_main` が 1 メッセージずつ取り出して `dispatch_*` を呼ぶ
- `Controller_init` (`apps/abcl_program.c:411`) が
  - GUI のスロット/スライダ/Start/Stop ボタンを `xinu_gui_*` で構築
  - 各アクタを ticker として登録 → WM が 16ms 毎に `tick` を投げる
- `Producer_tick` / `Consumer_tick` (本ファイル前半) が 3 状態の小さなステートマシン:
  1. `idle` (カウントダウン)
  2. `working`
  3. `waiting` (Buffer に `put`/`take` を発行して応答待ち)
- `Buffer_put` / `Buffer_take` は満杯/空チェックを行い `put_ok`/`put_full`/`take_ok`/`take_empty` で応答

GUI 連携 (`apps/abcl_xinu_gui.c`) は WM の毎フレーム描画フックに登録されており、producer / consumer の状態に応じて色が変わり、スライダで生産速度を変えたり Start/Stop ボタンが押せます。

**起動方法**: 既定の `main.c` ではこのデモは自動起動されません。動かすには abclc 経由で対応する `BoundedBuffer.abcl` をビルドする (もしくはこの C を直接リンクして `abcl_main` を呼ぶ) 必要があります。読み物としては以下の点に注目してください。

- `mailbox_t` (`apps/abcl_program.c:78-83`) — semaphore を 2 本使う古典的な有界キュー
- `abcl_enqueue` (`apps/abcl_program.c:119-139`) — 「`enqueue` という名前は queue.h の関数とぶつかるのでマクロで上書きしている」というコメントが本実装の注意点
- `dispatch` (`apps/abcl_program.c:461-469`) — クラス ID で分岐するメガディスパッチ
- `abcl_actor_main` (`apps/abcl_program.c:510-536`) — 各アクタが回す共通ループ。全体メッセージ上限 `_abcl_cap` を超えたら `abcl_shutdown()` で終わる安全弁あり

このソースは ABCL → C 翻訳でどんな C になるかの参考実装として大変有用です。ABCL の `class` がそのまま C の `dispatch_<Class>` 関数になり、`field` が `enum` インデックスになり、`send` が `enqueue` になる、という対応関係をたどれます。

---

## 11. 開発ワークフロー

典型的なセッション例:

```
# 1) 起動
$ ./compile/run-window.sh

# 2) 既存のサンプルを実行
xsh$ cd /home
xsh$ make
xsh$ ./hello
xsh$ ./sum

# 3) 自分の C を書く
xsh$ edit /home/myprog.c
        (C-x C-s で保存、C-x C-c で終了)
xsh$ cc myprog.c -o myprog
xsh$ ./myprog

# 4) ABCL を書く
xsh$ cd /home/abclcp/abclc
xsh$ edit MyActor.abcl
xsh$ abclc MyActor.abcl
xsh$ ./MyActor

# 5) WM を試す
xsh$ rotlines
xsh$ run /home/abclcp/abclc/RotLines
```

ヒント:

- 1 ターゲットだけビルドしたい場合は `make MyActor`
- すべて作り直したい場合は `rm Makefile && make`
- 一時ファイルを掃除したい場合は `rm *.c.bak` 等。XFS の inode は 32768 までなので大量作成は注意

---

## 12. トラブルシューティング

| 症状 | 原因 / 対処 |
| --- | --- |
| `qemu-system-arm not found` | `brew install qemu` (macOS) / `apt install qemu-system-arm` (Linux) |
| ビルドが古いまま起動する | `compile/` で `make clean && make PLATFORM=arm-qemu` |
| シェルプロンプトが出ない | `Ctrl-A → c` で QEMU モニタに入り `info registers` で停止位置を確認 |
| `cc: line N: ...` エラー | サポート外の構文 (構造体・ポインタ・配列・浮動小数 等) を使っていないか確認 |
| `abclc: translation failed` | `send` が 1 メソッドに 2 個ある、`var` 数オーバ、メソッド数オーバ等 |
| `command not found` だが実体はある | 暗黙 `run` は **絶対/相対パスのトークン1個** にしか効かない。引数を渡したい場合は `run PATH ARGS...` |
| WM ウィンドウが出ない | `arm-qemu` ビルドか? `run-window.sh` を使ったか? |
| ファイルが消えた | RAMDISK は揮発性。ホスト側にバックアップを |
| `xfs.h:171` を超えるパス | `XFS_PATH_MAX = 256`。長すぎるパスは切り詰められます |
| 画面が崩れた | エディタ中なら `C-l`、シェルなら `clear` |

---

## 付録 A. ファイルマップ

| 領域 | 主要ファイル |
| --- | --- |
| 起動 | `system/main.c`, `system/initialize.c`, `compile/run-*.sh` |
| XFS  | `include/xfs.h`, `system/xfs.c`, `device/ramdisk/`, `include/ramdisk.h` |
| cc   | `include/aout.h`, `system/cc.c`, `system/aout.c`, `shell/xsh_cc.c`, `shell/xsh_run.c` |
| abclc| `include/abclc.h`, `system/abclc.c`, `shell/xsh_abclc.c` |
| make/edit | `shell/xsh_make.c`, `shell/xsh_edit.c` |
| WM   | `apps/wm.c`, `apps/abcl_xinu_gui.c`, `shell/xsh_rotlines.c` |
| シェル | `shell/shell.c`, `shell/xsh_*.c`, `include/shell.h` |
| プラットフォーム | `compile/platforms/arm-qemu/`, `compile/platforms/arm-rpi/` |

## 付録 B. 既存サンプル一覧 (FS シード後)

```
/home/
├── hello.c
├── sum.c
└── abclcp/
    └── abclc/
        ├── PingPong.abcl
        └── RotLines.abcl
```

`make` を `cd /home && make` で走らせれば `hello`, `sum` が、
`cd /home/abclcp/abclc && make` で `PingPong`, `RotLines` が、それぞれ a.out として隣に生成されます。

---

以上で本拡張版 Xinu の使い方の概要は終わりです。さらに深く知りたい方は `system/cc.c` と `system/aout.c` (バイトコード VM の実体) や `system/abclc.c` (ABCL 翻訳) を読むのが近道です。
