#!/usr/bin/env python3
"""
plot_finalexam_v3.py
--------------------

Read csma_ca_v3.csv (produced by scratch/finalexam_v3.cc) and produce four
PNG figures evaluating CSMA/CA without RTS/CTS as the node count grows
from 2 to 30, plus a combined 4-panel summary.

Layout matches plots_finalexam_v2/ for visual consistency:

    1) throughput_vs_nodes.png
    2) pdr_vs_nodes.png
    3) collision_rate_vs_nodes.png
    4) delay_vs_nodes.png
    5) summary_4panel.png

After saving, the script opens all five images in the system's default
image viewer (GUI window), so the plots appear as pictures, not as text.

Usage:
    python3 plot_finalexam_v3.py             # reads ./csma_ca_v3.csv
    python3 plot_finalexam_v3.py path/to.csv # custom path
"""

import csv
import os
import subprocess
import sys
from pathlib import Path

import matplotlib

# Try an interactive backend first so plt.show() can open a GUI window.
# Fall back to Agg (file-only) if no display is available.
try:
    matplotlib.use("TkAgg")
    import tkinter  # noqa: F401  (probe)
    _HAS_GUI = True
except Exception:
    matplotlib.use("Agg")
    _HAS_GUI = False

import matplotlib.pyplot as plt


def read_csv(path: Path):
    nodes, offered, thr, delay, pdr, drops, retries, final_drops = (
        [], [], [], [], [], [], [], []
    )
    with open(path, newline="") as f:
        for row in csv.DictReader(f):
            nodes.append(int(row["Nodes"]))
            offered.append(float(row["OfferedMbps"]))
            thr.append(float(row["ThroughputMbps"]))
            delay.append(float(row["AvgDelayMs"]))
            pdr.append(float(row["PDRPercent"]))
            drops.append(float(row["PhyRxDrops"]))
            retries.append(float(row["MacRetries"]))
            final_drops.append(float(row["MacFinalDrops"]))
    return nodes, offered, thr, delay, pdr, drops, retries, final_drops


def compute_collision_rate(nodes, drops, offered_mbps,
                           sim_time=10.0, payload_bytes=1024):
    """
    Collision rate = PHY drops / (PHY drops + offered packets) per run.

    In v3 we drive every source at a low CBR rate (default 100 kbps), so
    OfferedMbps comes straight from the CSV and we convert it back to
    packets/run with sim_time and payload_bytes.
    """
    rates = []
    for n, d, off in zip(nodes, drops, offered_mbps):
        offered_per_run = off * 1e6 * sim_time / (payload_bytes * 8)
        if offered_per_run + d <= 0:
            rates.append(0.0)
        else:
            rates.append(100.0 * d / (offered_per_run + d))
    return rates


def style_axes(ax, title, xlabel, ylabel):
    ax.set_title(title, fontsize=13, fontweight="bold")
    ax.set_xlabel(xlabel, fontsize=11)
    ax.set_ylabel(ylabel, fontsize=11)
    ax.grid(True, linestyle="--", alpha=0.5)
    ax.set_xlim(min_x - 0.5, max_x + 0.5)


def save_and_collect(fig, outdir: Path, name: str, paths: list):
    out = outdir / name
    fig.tight_layout()
    fig.savefig(out, dpi=150)
    paths.append(out)
    print(f"  saved: {out}")


def open_image(path: Path):
    """Best-effort: open with the system default image viewer."""
    try:
        if sys.platform.startswith("linux"):
            subprocess.Popen(["xdg-open", str(path)],
                             stdout=subprocess.DEVNULL,
                             stderr=subprocess.DEVNULL)
        elif sys.platform == "darwin":
            subprocess.Popen(["open", str(path)])
        elif sys.platform == "win32":
            os.startfile(str(path))  # type: ignore[attr-defined]
    except Exception as e:
        print(f"  (could not auto-open {path.name}: {e})")


def main():
    csv_path = Path(sys.argv[1]) if len(sys.argv) > 1 else Path("csma_ca_v3.csv")
    if not csv_path.is_file():
        print(f"ERROR: CSV not found: {csv_path}", file=sys.stderr)
        sys.exit(1)

    outdir = csv_path.parent / "plots_finalexam_v3"
    outdir.mkdir(exist_ok=True)
    print(f"Reading {csv_path}")
    print(f"Writing PNGs to {outdir}/")

    (nodes, offered, thr, delay, pdr,
     drops, retries, final_drops) = read_csv(csv_path)
    collisions = compute_collision_rate(nodes, drops, offered)

    global min_x, max_x
    min_x, max_x = min(nodes), max(nodes)

    saved = []

    # 1) Throughput vs nodes
    fig, ax = plt.subplots(figsize=(8, 5))
    ax.plot(nodes, thr, marker="o", linewidth=2, color="#1f77b4",
            markersize=6, label="Aggregate throughput at sink")
    ax.plot(nodes, offered, linestyle="--", color="#888888", linewidth=1.4,
            label="Offered load")
    style_axes(ax, "Aggregate Throughput vs. Node Count\n"
                   "(802.11g 6 Mbps, ad-hoc, no RTS/CTS, under-saturated)",
               "Number of nodes (N)", "Throughput (Mbps)")
    ax.axhline(6.0, color="red", linestyle=":", alpha=0.6,
               label="PHY rate (6 Mbps)")
    ax.legend(loc="lower right")
    save_and_collect(fig, outdir, "throughput_vs_nodes.png", saved)

    # 2) PDR vs nodes
    fig, ax = plt.subplots(figsize=(8, 5))
    ax.plot(nodes, pdr, marker="s", linewidth=2, color="#2ca02c",
            markersize=6, label="Measured PDR")
    style_axes(ax, "Packet Delivery Ratio vs. Node Count",
               "Number of nodes (N)", "PDR (%)")
    ax.set_ylim(0, 105)
    ax.legend(loc="lower right")
    save_and_collect(fig, outdir, "pdr_vs_nodes.png", saved)

    # 3) Collision rate vs nodes
    fig, ax = plt.subplots(figsize=(8, 5))
    ax.plot(nodes, collisions, marker="^", linewidth=2, color="#d62728",
            markersize=6, label="PHY collision rate")
    style_axes(ax, "Collision Rate vs. Node Count",
               "Number of nodes (N)",
               "Collisions / (collisions + offered) [%]")
    ax.set_ylim(0, max(max(collisions) * 1.1, 5))
    ax.legend(loc="lower right")
    save_and_collect(fig, outdir, "collision_rate_vs_nodes.png", saved)

    # 4) Average delay vs nodes
    fig, ax = plt.subplots(figsize=(8, 5))
    ax.plot(nodes, delay, marker="D", linewidth=2, color="#9467bd",
            markersize=6, label="Average end-to-end delay")
    style_axes(ax, "Average End-to-End Delay vs. Node Count",
               "Number of nodes (N)", "Delay (ms)")
    ax.legend(loc="lower right")
    save_and_collect(fig, outdir, "delay_vs_nodes.png", saved)

    # 5) Combined 4-panel summary (matches v2 layout)
    if _HAS_GUI:
        combo, axs = plt.subplots(2, 2, figsize=(13, 9))
        axs[0, 0].plot(nodes, thr, "o-", color="#1f77b4")
        axs[0, 0].set_title("Throughput (Mbps)")
        axs[0, 1].plot(nodes, pdr, "s-", color="#2ca02c")
        axs[0, 1].set_title("PDR (%)")
        axs[1, 0].plot(nodes, collisions, "^-", color="#d62728")
        axs[1, 0].set_title("Collision rate (%)")
        axs[1, 1].plot(nodes, delay, "D-", color="#9467bd")
        axs[1, 1].set_title("Delay (ms)")
        for a in axs.flat:
            a.set_xlabel("Nodes")
            a.grid(True, linestyle="--", alpha=0.5)
        combo.suptitle("CSMA/CA without RTS/CTS — performance vs. node count "
                       "(v3, under-saturated)",
                       fontsize=14, fontweight="bold")
        combo.tight_layout()
        combo_path = outdir / "summary_4panel.png"
        combo.savefig(combo_path, dpi=150)
        saved.append(combo_path)
        print(f"  saved: {combo_path}")
        print("\nOpening GUI window with all 4 plots... close it to exit.")
        plt.show()
    else:
        # Headless: still produce the summary PNG, then try to open everything.
        combo, axs = plt.subplots(2, 2, figsize=(13, 9))
        axs[0, 0].plot(nodes, thr, "o-", color="#1f77b4")
        axs[0, 0].set_title("Throughput (Mbps)")
        axs[0, 1].plot(nodes, pdr, "s-", color="#2ca02c")
        axs[0, 1].set_title("PDR (%)")
        axs[1, 0].plot(nodes, collisions, "^-", color="#d62728")
        axs[1, 0].set_title("Collision rate (%)")
        axs[1, 1].plot(nodes, delay, "D-", color="#9467bd")
        axs[1, 1].set_title("Delay (ms)")
        for a in axs.flat:
            a.set_xlabel("Nodes")
            a.grid(True, linestyle="--", alpha=0.5)
        combo.suptitle("CSMA/CA without RTS/CTS — performance vs. node count "
                       "(v3, under-saturated)",
                       fontsize=14, fontweight="bold")
        combo.tight_layout()
        combo_path = outdir / "summary_4panel.png"
        combo.savefig(combo_path, dpi=150)
        saved.append(combo_path)
        print(f"  saved: {combo_path}")

        print("\nNo Tk display available; opening saved PNGs in image viewer.")
        for p in saved:
            open_image(p)

    print("\nDone.")


if __name__ == "__main__":
    main()
