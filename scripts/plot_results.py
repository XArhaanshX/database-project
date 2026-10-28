# /// script
# requires-python = ">=3.10"
# dependencies = ["matplotlib"]
# ///
"""Draws the figures used in the README and the paper from results/*.csv.

    uv run scripts/plot_results.py            # or: python3 scripts/plot_results.py

Writes PDF and PNG into results/figures/, and a copy of each PDF into
paper/figures/ (TeX engines such as tectonic only read files below the
document's own directory).
"""
import csv
import pathlib

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parent.parent
RESULTS = ROOT / "results"
FIGURES = RESULTS / "figures"
PAPER_FIGURES = ROOT / "paper" / "figures"

# Each variant keeps one colour, marker and line style in every figure.
# Colours are slots of a CVD-checked categorical palette; marker and dash
# carry identity as well, so the plots survive greyscale printing.
STYLE = {
    "baseline": dict(label="B+-tree (textbook)", color="#eb6834", marker="o", ls="--", hatch=""),
    "rightmost+insert-point": dict(
        label="rightmost cache + insert-point split", color="#1baf7a", marker="s", ls="-.", hatch="//"
    ),
    "adaptive-split": dict(label="adaptive split only", color="#eda100", marker="^", ls=":", hatch=".."),
    "sa-btree-16slots": dict(label="SA-B+-tree, 16 slots", color="#e34948", marker="v", ls=(0, (5, 1)), hatch=""),
    "sa-btree-f100": dict(label="SA-B+-tree, fill 1.0", color="#e87ba4", marker="D", ls=(0, (3, 1, 1, 1)), hatch=""),
    "sa-btree": dict(label="SA-B+-tree (ours)", color="#2a78d6", marker="o", ls="-", hatch="\\\\"),
}
INK = "#0b0b0b"
INK_2 = "#52514e"
GRID = "#e4e3df"

WORKLOAD_LABEL = {
    "sequential": "sequential",
    "random": "random",
    "streams-8": "8 streams",
    "streams-64": "64 streams",
    "nearly-sorted": "nearly sorted",
}

plt.rcParams.update(
    {
        "font.family": "serif",
        "font.serif": ["Nimbus Roman", "DejaVu Serif"],
        "font.size": 8,
        "axes.titlesize": 8.5,
        "axes.labelsize": 8,
        "axes.edgecolor": INK_2,
        "axes.labelcolor": INK,
        "axes.linewidth": 0.6,
        "xtick.color": INK_2,
        "ytick.color": INK_2,
        "xtick.labelsize": 7,
        "ytick.labelsize": 7,
        "legend.fontsize": 7,
        "legend.frameon": False,
        "pdf.fonttype": 42,
        "savefig.dpi": 220,
    }
)


def load(name):
    with open(RESULTS / f"{name}.csv", newline="") as f:
        return list(csv.DictReader(f))


def style_axes(ax):
    ax.grid(axis="y", color=GRID, linewidth=0.6)
    ax.set_axisbelow(True)
    for side in ("top", "right"):
        ax.spines[side].set_visible(False)


def save(fig, stem):
    for d in (FIGURES, PAPER_FIGURES):
        d.mkdir(parents=True, exist_ok=True)
        # No creation date, so re-running the script does not churn git.
        fig.savefig(d / f"{stem}.pdf", bbox_inches="tight", metadata={"CreationDate": None})
    fig.savefig(FIGURES / f"{stem}.png", bbox_inches="tight")
    plt.close(fig)


def fig_main(rows):
    """Grouped bars: workload x variant for three metrics."""
    variants = ["baseline", "rightmost+insert-point", "adaptive-split", "sa-btree"]
    workloads = list(WORKLOAD_LABEL)
    get = {(r["workload"], r["variant"]): r for r in rows}
    metrics = [
        ("reads_per_insert", "page reads per insert", 1.0),
        ("leaf_fill", "leaf fill (%)", 100.0),
        ("insert_ns", "insert time (ns)", 1.0),
    ]
    fig, axes = plt.subplots(1, 3, figsize=(7.16, 2.15))
    width = 0.8 / len(variants)
    for ax, (col, title, scale) in zip(axes, metrics):
        for i, v in enumerate(variants):
            s = STYLE[v]
            xs = [w + (i - (len(variants) - 1) / 2) * width for w in range(len(workloads))]
            ys = [float(get[(w, v)][col]) * scale for w in workloads]
            ax.bar(xs, ys, width, color=s["color"], edgecolor="white", linewidth=0.8,
                   hatch=s["hatch"], label=s["label"])
        ax.set_title(title, color=INK)
        ax.set_xticks(range(len(workloads)))
        ax.set_xticklabels([WORKLOAD_LABEL[w] for w in workloads], rotation=30, ha="right")
        ax.tick_params(axis="x", length=0)
        style_axes(ax)
    axes[1].set_ylim(0, 105)
    axes[2].set_yscale("log")
    axes[2].set_ylim(5, 400)
    axes[2].yaxis.set_major_formatter(matplotlib.ticker.FuncFormatter(lambda y, _: f"{y:g}"))
    handles, labels = axes[0].get_legend_handles_labels()
    fig.legend(handles, labels, loc="upper center", ncol=4, bbox_to_anchor=(0.5, 1.08),
               handlelength=1.6, columnspacing=1.4)
    fig.tight_layout(w_pad=1.2)
    save(fig, "summary")


def end_label(ax, x, y, text, color, dy=0):
    ax.annotate(text, (x, y), xytext=(4, dy), textcoords="offset points", va="center",
                fontsize=6.5, color=INK_2)


def fig_streams(rows):
    """Lines over the number of interleaved streams.

    Curves that coincide by construction are left out of a panel: the
    adaptive split alone reads exactly like the textbook tree, and every
    adaptive variant has the same fill whatever its hint cache size.
    """
    by_v = {}
    for r in rows:
        by_v.setdefault(r["variant"], []).append((int(r["workload"].split("-")[1]), r))
    fig, axes = plt.subplots(1, 2, figsize=(7.16, 2.2))
    panels = [
        ("reads_per_insert", "page reads per insert", 1.0, (0.8, 3.2),
         ["baseline", "rightmost+insert-point", "sa-btree-16slots", "sa-btree"]),
        ("leaf_fill", "leaf fill (%)", 100.0, (40, 102),
         ["baseline", "rightmost+insert-point", "sa-btree"]),
    ]
    for ax, (col, title, scale, ylim, variants) in zip(axes, panels):
        for v in variants:
            s = STYLE[v]
            pts = sorted(by_v[v], key=lambda p: p[0])
            xs = [p[0] for p in pts]
            ys = [float(p[1][col]) * scale for p in pts]
            ax.plot(xs, ys, color=s["color"], ls=s["ls"], lw=1.4, marker=s["marker"], ms=3.5,
                    markeredgecolor="white", markeredgewidth=0.5, label=s["label"])
        ax.set_xscale("log", base=2)
        ax.set_xticks([1, 4, 16, 64, 256, 1024])
        ax.get_xaxis().set_major_formatter(matplotlib.ticker.ScalarFormatter())
        ax.set_xlabel("number of interleaved ascending streams")
        ax.set_title(title, color=INK)
        ax.set_ylim(*ylim)
        style_axes(ax)
    for slots in (16, 64):
        axes[0].axvline(slots, color=GRID, lw=0.8, zorder=0)
        axes[0].annotate(f"{slots} slots", (slots, 2.2), xytext=(-3, 0), textcoords="offset points",
                         ha="right", fontsize=6.5, color=INK_2)
    handles, labels = axes[0].get_legend_handles_labels()
    fig.legend(handles, labels, loc="upper center", ncol=4, bbox_to_anchor=(0.5, 1.09),
               handlelength=2.4, columnspacing=1.4)
    fig.tight_layout(w_pad=1.5)
    save(fig, "streams")


def fig_late(rows):
    """Leaf fill and index size over the share of late records."""
    variants = ["baseline", "rightmost+insert-point", "sa-btree-f100", "sa-btree"]
    by_v = {v: [] for v in variants}
    for r in rows:
        if r["variant"] in by_v:
            by_v[r["variant"]].append((float(r["workload"].split("-")[1]) * 100, r))
    fig, ax = plt.subplots(figsize=(3.5, 2.5))
    for v in variants:
        s = STYLE[v]
        pts = sorted(by_v[v], key=lambda p: p[0])
        xs = [p[0] for p in pts]
        ys = [float(p[1]["leaf_fill"]) * 100 for p in pts]
        ax.plot(xs, ys, color=s["color"], ls=s["ls"], lw=1.4, marker=s["marker"], ms=3.5,
                markeredgecolor="white", markeredgewidth=0.5, label=s["label"])
    ax.set_xlabel("records arriving late (%)")
    ax.set_ylabel("leaf fill (%)")
    ax.set_ylim(40, 105)
    ax.set_xticks([0, 1, 2, 5, 10, 20])
    style_axes(ax)
    ax.legend(loc="lower center", bbox_to_anchor=(0.5, 1.0), ncol=2, handlelength=2.4,
              columnspacing=1.0)
    fig.tight_layout()
    save(fig, "late")


def main():
    fig_main(load("main"))
    fig_streams(load("streams_sweep"))
    fig_late(load("late_sweep"))
    print("figures written to", FIGURES.relative_to(ROOT))


if __name__ == "__main__":
    main()
