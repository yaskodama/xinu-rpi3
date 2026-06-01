#!/usr/bin/env python3
"""Dining philosophers ベンチ結果 JSON を日本語 LaTeX/PDF レポートに変換。

使い方:
    dining_bench.py --out docs/dining_results.json
    build_dining_report.py docs/dining_results.json \
       --out docs/dining_report.tex --pdf docs/dining_report.pdf
"""
import argparse
import datetime
import json
import os
import subprocess
import sys

PROJECT_ROOT = os.path.abspath(os.path.join(
    os.path.dirname(os.path.abspath(__file__)), ".."))

SOURCE_EXCERPTS = [
    ("examples/dining5_bench.abcl",   23, 38, "Fork クラス (acquire/release)"),
    ("examples/dining5_bench.abcl",   40, 91, "Philosopher クラス (FSM)"),
    ("examples/dining5_bench.abcl",   93, 184, "DiningBench (Orchestrator) クラス"),
    ("examples/dining5_bench.abcl",  186, 189, "global (actor 識別子のみ)"),
]

TEX_TEMPLATE = r"""\documentclass[a4paper,11pt]{ltjsarticle}
\usepackage[margin=22mm]{geometry}
\usepackage{booktabs}
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

\lstdefinelanguage{AIPL}{
  morekeywords={class,method,function,var,new,send,now,future,
                if,else,while,do,return,suicide,await,sender,self,this},
  morekeywords=[2]{int,any,float,string,bool},
  morecomment=[l]{//},
  morecomment=[s]{/*}{*/},
  morestring=[b]"
}

\title{5 哲学者問題: 分散戦略ベンチマーク \\
  \large Raspberry Pi 3 / Embedded Xinu / AIPL アクター (50 食 × 5 人)}
\author{自動生成: \texttt{tools/build\_dining\_report.py}}
\date{__DATE__}

\begin{document}
\maketitle

\begin{abstract}
古典的な 5 哲学者問題を Embedded Xinu (Raspberry Pi 3 B+) の AIPL アクター
ランタイム上で実装し、5 人全員が各 __MEALS__ 食終わるまでの壁時計時間を、
3 種類の起動戦略で比較する:
\textbf{(a) parallel}: 5 人全員を同時起動。
\textbf{(b) staggered(3+2)}: P0..P2 を起動 → 3 人完食後に P3,P4 を起動。
\textbf{(c) sequential}: 1 人ずつ順番に起動 (前の人が完食してから次が始まる)。
GC アクター (Collector) を有効化し、デッドロック回避のため P4 は低 ID 側
(F0) から先にフォークを取る canonical asymmetric pattern を採用している。
\end{abstract}

\section{実行戦略}

5 哲学者と 5 フォークが円卓上に並ぶ古典構成だが、本ベンチでは
\emph{哲学者の起動タイミング} を変化させて並列度の差を測る:

\begin{description}
  \item[parallel] 全 5 アクターを同時に \texttt{init\_phil} で起動。最大並列度。
        フォーク争奪の競合に飲み込まれない限り、最速になるはず。
  \item[staggered(3+2)] 最初に P0, P1, P2 を起動。3 人とも 50 食を終えた
        瞬間に P3, P4 を起動。前半は並列度 3、後半は並列度 2。
  \item[sequential] P0 完食 → P1 起動 → P1 完食 → P2 起動 ... の完全直列。
        並列度 1、所要時間 $\approx 5 \times$ 一人分の時間 になるはず。
\end{description}

各戦略を __REPEAT__ 回反復し、中央値を採用する。所要時間は Pi 3 内部の
\texttt{clkticks * 10} で計測 (10 ms 粒度)。

\section{結果}

\begin{table}[h]
\centering
\begin{tabular}{lccccc}
\toprule
mode & ラベル & 全 5 完食 (ms) & 最遅 1 人 (ms) & 反復回数 & 反復標本 (ms) \\
\midrule
__TABLE_ROWS__
\bottomrule
\end{tabular}
\caption{各起動戦略の壁時計中央値。「全 5 完食」が最終目標。
「最遅 1 人」はその run で一番遅かった哲学者の所要時間
(各哲学者の起動 $\to$ suicide 通知までの差)。
反復標本欄は raw measurement の列挙。}
\label{tab:dining}
\end{table}

\begin{figure}[h]
\centering
\begin{tikzpicture}
\begin{axis}[
    width=12cm, height=7cm,
    ybar,
    bar width=20pt,
    ylabel={全 5 完食 中央値 (ms)},
    symbolic x coords={__X_COORDS__},
    xtick=data,
    enlarge x limits=0.25,
    nodes near coords,
    nodes near coords align={vertical},
    nodes near coords style={font=\footnotesize},
]
\addplot[fill=blue!40,draw=blue] coordinates {__PLOT_DATA__};
\end{axis}
\end{tikzpicture}
\caption{戦略別の壁時計時間。並列度が高いほど短くなることを観察。}
\label{fig:dining}
\end{figure}

\section{考察}

__OBSERVATIONS__

\section{AIPL ソースコード}

完全版は \texttt{examples/dining5\_bench.abcl}。Pi 3 上で実際に走る C 実装は
\texttt{apps/abcl\_program.c} 内に手翻訳されている (xinu-raz には aipl2c が
無いため)。振る舞いは AIPL 仕様と等価。

__SOURCE_LISTINGS__

\section{再現手順}

\begin{lstlisting}[language=bash]
# 1. ビルド + flash (SD swap)
cd ~/projects/xinu-raz/xinu/compile && make PLATFORM=arm-rpi3

# 2. Pi 3 起動後、GC + dining actor を init:
curl -X POST http://192.168.3.50:8080/api/gc-actor/init
curl -X POST http://192.168.3.50:8080/api/dining/init

# 3. ベンチを走らせて PDF を生成
cd ~/projects/xinu-raz/xinu/tools
./dining_bench.py --meals 50 --repeat 3 \
    --out docs/dining_results.json
./build_dining_report.py docs/dining_results.json \
    --out docs/dining_report.tex --pdf docs/dining_report.pdf
\end{lstlisting}

\section{生データ (JSON)}

\begin{lstlisting}
__RAW_JSON__
\end{lstlisting}

\end{document}
"""


def latex_escape(s):
    return (s.replace("\\", r"\textbackslash{}")
             .replace("_", r"\_")
             .replace("&", r"\&")
             .replace("%", r"\%")
             .replace("$", r"\$")
             .replace("#", r"\#")
             .replace("{", r"\{")
             .replace("}", r"\}"))


def lang_for(path):
    if path.endswith(".c"):    return "C"
    if path.endswith(".py"):   return "Python"
    if path.endswith(".sh"):   return "bash"
    if path.endswith(".abcl"): return "AIPL"
    return ""


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
        loc = latex_escape(f"{relpath}:{start}-{end}")
        parts.append(
            f"\\paragraph{{{label}}} \\hfill \\texttt{{{loc}}}\n"
            f"\\lstinputlisting[firstline={start},lastline={end}"
            f"{firstnum}{langspec}]{{{abspath}}}\n")
    return "\n".join(parts)


def render(data):
    results = data["results"]
    repeat  = data["repeat"]
    meals   = data["meals"]

    rows = []
    plot_pts = []
    x_coords = []
    for r in results:
        label = r.get("label", str(r["mode"]))
        x_coords.append(label.replace(" ", ""))
        rows.append(
            f"{r['mode']} & {latex_escape(label)} & {r['median_ms']} & "
            f"{r['median_max_phil_ms']} & {repeat} & "
            f"{latex_escape(str(r['samples_ms']))} \\\\")
        plot_pts.append((label.replace(" ", ""), r["median_ms"]))

    plot_data = " ".join(f"({x},{y})" for x, y in plot_pts)

    # 観察を自動生成
    obs = []
    medians = {r["mode"]: r["median_ms"] for r in results}
    if 0 in medians and 2 in medians and medians[0] > 0:
        ratio = medians[2] / medians[0]
        obs.append(f"sequential mode の壁時計は parallel mode の "
                   f"$\\approx {ratio:.2f}\\times$。並列度 1 vs 5 を考えると "
                   f"理論上 5$\\times$ 近いはずだが、フォーク争奪・"
                   f"Xinu スケジューラのコンテキストスイッチコスト・"
                   f"actor mailbox の処理オーバヘッドにより 5$\\times$ 未満に収まる。")
    if 0 in medians and 1 in medians:
        if medians[1] > medians[0]:
            obs.append(f"staggered(3+2) は parallel より "
                       f"{medians[1] / medians[0]:.2f}$\\times$ 遅い。"
                       f"後半 2 人 (P3, P4) のフェーズで並列度が落ちるため。")
        else:
            obs.append(f"staggered(3+2) と parallel が同等以下。"
                       f"前半のフォーク争奪が緩むことで残り 2 人の独走が "
                       f"効率的に進んだケース。")
    for r in results:
        if r["median_ms"] == r["median_max_phil_ms"]:
            obs.append(f"{r['label']} では \"全 5 完食\" と \"最遅 1 人\" が "
                       f"等しく、最後の哲学者の終了で全体が決まっている "
                       f"(古典 critical path)。")
    obs_tex = ("\\begin{itemize}\n"
               + "\n".join(f"  \\item {o}" for o in obs)
               + "\n\\end{itemize}")

    today = datetime.date.today().isoformat()
    tex = TEX_TEMPLATE
    tex = tex.replace("__DATE__",  today)
    tex = tex.replace("__MEALS__", str(meals))
    tex = tex.replace("__REPEAT__", str(repeat))
    tex = tex.replace("__TABLE_ROWS__", "\n".join(rows))
    tex = tex.replace("__X_COORDS__", ",".join(x_coords))
    tex = tex.replace("__PLOT_DATA__", plot_data)
    tex = tex.replace("__OBSERVATIONS__", obs_tex)
    tex = tex.replace("__SOURCE_LISTINGS__", build_source_listings())
    tex = tex.replace("__RAW_JSON__", json.dumps(data, indent=2))
    return tex


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("results_json")
    ap.add_argument("--out", required=True)
    ap.add_argument("--pdf")
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
    for _ in range(2):
        r = subprocess.run(
            ["lualatex", "-interaction=nonstopmode", "-halt-on-error",
             basename + ".tex"],
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
