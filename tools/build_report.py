#!/usr/bin/env python3
"""N-Queens ベンチマーク結果 JSON を日本語 LaTeX レポートに変換し、
lualatex で PDF を生成する。

使い方:
    nqueens_bench.py --out results.json
    build_report.py results.json --out report.tex --pdf report.pdf
"""
import argparse
import datetime
import json
import os
import subprocess
import sys

# プロジェクトのルート (このスクリプトの 1 つ上のディレクトリ)
PROJECT_ROOT = os.path.abspath(os.path.join(
    os.path.dirname(os.path.abspath(__file__)), ".."))

# レポートに埋め込むソースコード抜粋の (相対パス, 開始行, 終了行, ラベル)
SOURCE_EXCERPTS = [
    ("apps/abcl_program.c", 1505, 1531, "N-Queens 計算カーネル (C, Pi 3 側)"),
    ("apps/abcl_program.c", 1400, 1432, "Worker.compute\\_nq アクターメソッド"),
    ("apps/abcl_program.c", 1118, 1147, "Dispatcher.submit\\_nq (Round-robin 振分け)"),
    ("apps/webactor.c",     1422, 1517, "/api/loadbal/nqueens HTTP ルート"),
    ("tools/nqueens_bench.py",  37,  61, "Mac 側 N-Queens 参照実装 (Python)"),
    ("tools/nqueens_bench.py", 153, 184, "Mac 側 Pi 3 ポーリング (並列)"),
]

TEX_TEMPLATE = r"""\documentclass[a4paper,11pt]{ltjsarticle}
\usepackage[margin=22mm]{geometry}
\usepackage{booktabs}
\usepackage{graphicx}
\usepackage{pgfplots}
\pgfplotsset{compat=1.18}
\usepackage{listings}
\usepackage{xcolor}
\usepackage{hyperref}
\hypersetup{colorlinks=true,linkcolor=blue,urlcolor=blue}

\lstset{
  basicstyle=\ttfamily\footnotesize,
  backgroundcolor=\color{black!4},
  frame=single,
  framerule=0pt,
  breaklines=true,
  showstringspaces=false,
  numbers=left,
  numberstyle=\tiny\color{gray},
  numbersep=6pt,
  xleftmargin=12pt,
  keywordstyle=\color{blue!70!black}\bfseries,
  commentstyle=\color{green!40!black}\itshape,
  stringstyle=\color{red!60!black}
}

\title{N-Queens 分散ベンチマーク \\
  \large Mac + Raspberry Pi 3 (Embedded Xinu, AIPL アクター負荷分散器)}
\author{自動生成: \texttt{tools/build\_report.py}}
\date{__DATE__}

\begin{document}
\maketitle

\begin{abstract}
本レポートは N-Queens 問題 (盤面サイズ $N \in$ \{__SIZES__\}) の解の総数を
数える処理を題材に、3 つの実行方式の壁時計時間を比較する:
\textbf{(a)} Mac 単独 (純粋 Python の参照実装)、
\textbf{(b)} Pi 3 単独 (Embedded Xinu 上の C 実装を 4 並列の AIPL ワーカー
アクターで実行、最少負荷ディスパッチャ経由)、
\textbf{(c)} Mac + Pi 3 分散 (first\_col の低い側を Mac、高い側を Pi 3 が
並列に処理)。Pi 3 は Raspberry Pi 3 B+ (BCM2837) で、MMU 有効、定期 GC
アクターも動作中。タスクは HTTP POST \texttt{/api/loadbal/nqueens} 経由
で投入、結果は \texttt{/api/loadbal/task?id=N} でポーリング取得する。
\end{abstract}

\section{システム構成}

Pi 3 側ランタイム (\texttt{xinu-rpi3}, ブランチ \texttt{arm-rpi3-port}) の構成:
\begin{itemize}
  \item Embedded Xinu カーネル + ARMv7-A short-descriptor MMU 有効化、
        Normal cacheable RAM と Device strongly-ordered MMIO を識別マップ
        で領域属性分離。
  \item AIPL アクターシステム (Lock-free MPSC メールボックス)。
  \item \texttt{CLASS\_Dispatcher} アクター (優先度 25): 着信
        \texttt{compute\_nq(n, first\_col)} を 4 つの
        \texttt{CLASS\_Worker} アクター (優先度 22) にラウンドロビンで振分け。
  \item \texttt{CLASS\_Collector} アクター (優先度 26): 定期的に休眠
        アクターを掃除し、in-memory タスクテーブルの期限切れ PENDING
        エントリも検出してキャンセル状態に遷移させる。
  \item HTTP サーバ (port 8080): 負荷分散制御 (\texttt{/api/loadbal/*})、
        GC アクター制御 (\texttt{/api/gc-actor/*}) を公開。
\end{itemize}

Pi 3 の N-Queens 計算カーネルは $\mathrm{O}(N!)$ 再帰の解数カウンタで、
Mac 側は同じアルゴリズムを Python で実装している。最初の行 (row 0) の
クイーン列で分割する方式は embarrassingly parallel で、各 \texttt{first\_col}
の部分木は独立かつほぼ等コスト。

\section{方法}

各盤面サイズ $N \in$ \{__SIZES__\} と各モードについて、ワークロードを
__REPEAT__ 回連続実行して \emph{中央値} を採用する (平均はどちらかの
GC ポーズで歪みやすい)。解の総数は既知値 (N=8 で 92、N=9 で 352、
N=10 で 724、N=11 で 2680) と照合し、ずれた場合は JSON に明記する。

\subsection{3 つのモード}
\begin{description}
  \item[\texttt{mac}] Mac 上の純粋 Python。
  \item[\texttt{pi3}] 全 first\_col を Pi 3 の
        \texttt{/api/loadbal/nqueens} に投入、ポーリングで完了収集。
  \item[\texttt{split}] Mac が \texttt{first\_col} $\in [0, N/2)$ を、
        Pi 3 が $[N/2, N)$ を担当。Mac 側
        \texttt{ThreadPoolExecutor} で両半分を同時実行 — これが
        「Mac + Xinu 分散」のケースである。
\end{description}

\section{結果}

\begin{table}[h]
\centering
\begin{tabular}{cccccc}
\toprule
$N$ & 解の数 & Mac (ms) & Pi 3 (ms) & 分散 (ms) & 高速化率 (Mac/分散) \\
\midrule
__TABLE_ROWS__
\bottomrule
\end{tabular}
\caption{__REPEAT__ 回反復の中央値 (壁時計時間)。
高速化率列は Mac 単独に対する Mac+Pi 3 分散の倍率で、$> 1.0$ なら分散の方が
速い。}
\label{tab:results}
\end{table}

\begin{figure}[h]
\centering
\begin{tikzpicture}
\begin{axis}[
    width=12cm, height=7cm,
    ybar,
    bar width=8pt,
    ymode=log,
    log basis y={10},
    ylabel={壁時計時間 (ms、対数軸)},
    symbolic x coords={__X_COORDS__},
    xtick=data,
    enlarge x limits=0.15,
    legend pos=north west,
    nodes near coords,
    nodes near coords align={vertical},
    nodes near coords style={font=\tiny},
]
__PLOT_DATA__
\end{axis}
\end{tikzpicture}
\caption{盤面サイズ $N$ に対する中央壁時計時間 (Y 軸対数表示)。}
\label{fig:results}
\end{figure}

\section{考察}

__OBSERVATIONS__

\section{ソースコード}

ベンチマークが実行する主要なコード断片を以下に示す。完全版は
GitHub リポジトリ \url{https://github.com/yaskodama/xinu-rpi3} の
ブランチ \texttt{arm-rpi3-port} 参照。

__SOURCE_LISTINGS__

\section{再現手順}

\begin{lstlisting}[language=bash]
# 1. Pi 3 カーネルをビルドして焼く
cd ~/projects/xinu-raz/xinu/compile && make PLATFORM=arm-rpi3

# 2. Pi 3 起動後 (SD swap または /kexec)、アクターを明示的に起動:
curl -X POST http://192.168.3.50:8080/api/loadbal/init
curl -X POST http://192.168.3.50:8080/api/gc-actor/init

# 3. ベンチマーク実行 + レポート生成
cd ~/projects/xinu-raz/xinu/tools
./nqueens_bench.py --sizes __SIZES__ --pretty --out results.json
./build_report.py results.json --out report.tex --pdf report.pdf
\end{lstlisting}

\section{生データ (JSON)}

\begin{lstlisting}
__RAW_JSON__
\end{lstlisting}

\end{document}
"""


def cell_lookup(results, mode, n):
    for r in results:
        if r["mode"] == mode and r["n"] == n:
            if "error" in r:
                return None
            return r["median_ms"], r.get("count")
    return None


def lang_for(path):
    if path.endswith(".c"):  return "C"
    if path.endswith(".py"): return "Python"
    if path.endswith(".sh"): return "bash"
    return ""


def latex_escape(s):
    """\\texttt や本文中で安全に出せるよう特殊文字をエスケープ。"""
    return (s.replace("\\", r"\textbackslash{}")
             .replace("_", r"\_")
             .replace("&", r"\&")
             .replace("%", r"\%")
             .replace("$", r"\$")
             .replace("#", r"\#")
             .replace("{", r"\{")
             .replace("}", r"\}"))


def build_source_listings():
    parts = []
    for relpath, start, end, label in SOURCE_EXCERPTS:
        abspath = os.path.join(PROJECT_ROOT, relpath)
        if not os.path.exists(abspath):
            parts.append(
                f"\\paragraph{{{label}}} ({latex_escape(relpath)} not found)\n")
            continue
        lang = lang_for(relpath)
        firstnum = f", firstnumber={start}"
        langspec = f", language={lang}" if lang else ""
        loc_label = latex_escape(f"{relpath}:{start}-{end}")
        parts.append(
            f"\\paragraph{{{label}}} \\hfill\n"
            f"\\texttt{{{loc_label}}}\n"
            f"\\lstinputlisting[firstline={start},lastline={end}"
            f"{firstnum}{langspec}]{{{abspath}}}\n"
        )
    return "\n".join(parts)


def render(data):
    results = data["results"]
    repeat  = data["repeat"]
    sizes   = sorted({r["n"] for r in results})

    rows, plot_mac, plot_pi3, plot_split = [], [], [], []
    for n in sizes:
        mac = cell_lookup(results, "mac",   n)
        pi3 = cell_lookup(results, "pi3",   n)
        spl = cell_lookup(results, "split", n)
        sol = (mac or pi3 or spl or (None, None))[1]
        sol_s = str(sol) if sol is not None else "--"
        mac_s = str(mac[0]) if mac else "--"
        pi3_s = str(pi3[0]) if pi3 else "--"
        spl_s = str(spl[0]) if spl else "--"
        if mac and spl and spl[0] > 0:
            sp = f"{mac[0] / spl[0]:.2f}"
        else:
            sp = "--"
        rows.append(f"{n} & {sol_s} & {mac_s} & {pi3_s} & {spl_s} & {sp} \\\\")
        if mac: plot_mac.append((n, mac[0]))
        if pi3: plot_pi3.append((n, pi3[0]))
        if spl: plot_split.append((n, spl[0]))

    def plot_series(name, data, color):
        coords = " ".join(f"({n},{ms})" for n, ms in data)
        return (f"\\addplot[fill={color}!40,draw={color}] "
                f"coordinates {{{coords}}};\n"
                f"\\addlegendentry{{{name}}}")

    plot_data = "\n".join([
        plot_series("Mac (Python)",       plot_mac,   "blue"),
        plot_series("Pi 3 (4 workers)",   plot_pi3,   "red"),
        plot_series("Mac + Pi 3 分散",     plot_split, "green"),
    ])

    obs = []
    for n in sizes:
        mac = cell_lookup(results, "mac",   n)
        pi3 = cell_lookup(results, "pi3",   n)
        spl = cell_lookup(results, "split", n)
        if mac and pi3:
            ratio = pi3[0] / mac[0] if mac[0] > 0 else 0
            obs.append(
                f"$N={n}$ では Pi 3 (4 ワーカー、1.4 GHz Cortex-A53 / "
                f"AArch32) が {pi3[0]} ms、Mac は {mac[0]} ms "
                f"($\\approx {ratio:.1f}\\times$)。")
        if mac and spl and spl[0] > 0 and mac[0] > 0:
            speedup = mac[0] / spl[0]
            if speedup > 1.05:
                obs.append(
                    f"$N={n}$ で Mac+Pi 3 分散は Mac 単独より "
                    f"{speedup:.2f}$\\times$ 高速。")
            elif speedup < 0.95:
                obs.append(
                    f"$N={n}$ では Mac+Pi 3 分散は逆に "
                    f"{speedup:.2f}$\\times$ 遅い。本サイズでは HTTP "
                    f"往復 + JSON ポーリングのオーバヘッドが実計算時間を "
                    f"完全に支配しているため。")
    obs_tex = ("\\begin{itemize}\n"
               + "\n".join(f"  \\item {o}" for o in obs)
               + "\n\\end{itemize}")

    today = datetime.date.today().isoformat()

    tex = TEX_TEMPLATE
    tex = tex.replace("__DATE__", today)
    tex = tex.replace("__SIZES__", ", ".join(str(s) for s in sizes))
    tex = tex.replace("__REPEAT__", str(repeat))
    tex = tex.replace("__TABLE_ROWS__", "\n".join(rows))
    tex = tex.replace("__X_COORDS__", ",".join(str(s) for s in sizes))
    tex = tex.replace("__PLOT_DATA__", plot_data)
    tex = tex.replace("__OBSERVATIONS__", obs_tex)
    tex = tex.replace("__SOURCE_LISTINGS__", build_source_listings())
    tex = tex.replace("__RAW_JSON__", json.dumps(data, indent=2))
    return tex


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("results_json")
    ap.add_argument("--out", required=True, help="出力する .tex のパス")
    ap.add_argument("--pdf", help="lualatex を実行してこの PDF も生成する")
    args = ap.parse_args()

    with open(args.results_json) as f:
        data = json.load(f)

    tex = render(data)
    with open(args.out, "w") as f:
        f.write(tex)
    print(f"wrote {args.out}", file=sys.stderr)

    if not args.pdf:
        return 0

    workdir  = os.path.dirname(os.path.abspath(args.out)) or "."
    basename = os.path.basename(args.out).rsplit(".", 1)[0]
    # lualatex を 2 回走らせる (図表参照を解決させるため)
    for i in range(2):
        r = subprocess.run(
            ["lualatex", "-interaction=nonstopmode",
             "-halt-on-error", basename + ".tex"],
            cwd=workdir, capture_output=True, text=True)
        if r.returncode != 0:
            sys.stderr.write(r.stdout)
            sys.stderr.write(r.stderr)
            return 1
    out_pdf = os.path.join(workdir, basename + ".pdf")
    if os.path.abspath(out_pdf) != os.path.abspath(args.pdf):
        os.replace(out_pdf, args.pdf)
    print(f"wrote {args.pdf}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
