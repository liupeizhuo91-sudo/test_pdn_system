#!/usr/bin/env python3
"""Generate plots for distributed PDN experiment CSV outputs.

The script reads result folders produced by build/Makefile and writes a compact
HTML gallery plus PNG figures under results/figures by default.
"""

import argparse
import csv
import math
import re
from pathlib import Path


CASES = (
    "full",
    "no_learning",
    "no_eec",
    "no_balance",
    "baseline_all_off",
)

VOLTAGE_METRICS = (
    ("droop_mv", "droop"),
    ("overshoot_mv", "overshoot"),
    ("pkpk_mv", "pkpk"),
)

POWER_METRICS = (
    ("pin_mw", "Pin"),
    ("pout_mw", "Pout"),
)

AUX_POWER_METRICS = (
    ("control_power_mw", "control"),
    ("guardband_power_mw", "guardband"),
    ("switching_power_mw", "switching"),
)

LEARNING_METRICS = (
    ("vrefh_mv", "VREFH"),
    ("vrefl_mv", "VREFL"),
    ("vdrp_mv", "VDRP"),
    ("vos_mv", "VOS"),
)

PHASE_LABELS = (
    "VREFL",
    "VREFH",
    "CTieHi",
    "CTieLo",
    "Done",
)

CASE_SUMMARY_METRICS = (
    ("avg_pin_mw", "Average input power (mW)"),
    ("avg_efficiency_pct", "Average efficiency (%)"),
    ("best_droop_mv", "Best droop (mV)"),
    ("best_pkpk_mv", "Best peak-to-peak (mV)"),
    ("best_pkpk_iteration", "Best peak-to-peak iteration"),
    ("droop_at_best_pkpk_mv", "Droop at best peak-to-peak iteration (mV)"),
    ("droop_reduction_at_best_pkpk_pct",
     "Droop reduction at best peak-to-peak iteration (%)"),
    ("avg_guardband_power_mw", "Average guardband power (mW)"),
    ("avg_switching_power_mw", "Average switching power (mW)"),
)

BENEFIT_CASES = (
    ("no_eec", "w/o EEC", "#f6a04d", "#d46d1c"),
    ("no_learning", "w/o Learning", "#f3cf59", "#b78e11"),
    ("full", "w/ Learning", "#a8bf72", "#6f8d42"),
)

PAPER_METRICS = (
    "baseline_max_droop",
    "best_max_droop",
    "droop_reduction",
    "best_current_stddev",
    "best_efficiency",
    "energy_saving_min",
    "energy_saving_max",
    "performance_improvement_min",
    "performance_improvement_max",
)


def import_matplotlib():
    try:
        import matplotlib

        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        import numpy as np
    except ImportError as exc:
        raise SystemExit(
            "matplotlib and numpy are required for visualization. "
            "Install them in the Python environment used by Makefile."
        ) from exc
    return plt, np


def read_rows(path):
    if not path.exists():
        return []
    with path.open(newline="") as f:
        return list(csv.DictReader(f))


def to_float(row, key):
    value = row.get(key, "")
    if value == "":
        return math.nan
    try:
        return float(value)
    except ValueError:
        return math.nan


def finite_values(rows, key):
    values = [to_float(row, key) for row in rows]
    return [value for value in values if math.isfinite(value)]


def slug(value):
    value = value.strip().lower()
    value = re.sub(r"[^a-z0-9]+", "_", value)
    return value.strip("_") or "plot"


def group_by(rows, key):
    groups = {}
    for row in rows:
        groups.setdefault(row.get(key, ""), []).append(row)
    return groups


def sorted_by_float(rows, key):
    return sorted(rows, key=lambda row: to_float(row, key))


def sparsity_value(scenario):
    match = re.search(r"(?:^|_)(\d+(?:\.\d+)?)pct", scenario)
    if not match:
        return math.nan
    return float(match.group(1))


def is_sparsity_scenario(scenario):
    return scenario != "overall_ai" and math.isfinite(sparsity_value(scenario))


def set_common_style(ax, title, xlabel, ylabel):
    ax.set_title(title)
    ax.set_xlabel(xlabel)
    ax.set_ylabel(ylabel)
    ax.grid(True, alpha=0.25)


def save_line_plot(plt, out_path, rows, x_key, series, title, ylabel):
    rows = sorted_by_float(rows, x_key)
    x = finite_values(rows, x_key)
    if not x:
        return False

    fig, ax = plt.subplots(figsize=(8.5, 4.5))
    plotted = False
    for key, label in series:
        y = [to_float(row, key) for row in rows]
        if any(math.isfinite(value) for value in y):
            ax.plot(x, y, marker="o", markersize=2.5, linewidth=1.4, label=label)
            plotted = True
    if not plotted:
        plt.close(fig)
        return False

    set_common_style(ax, title, x_key, ylabel)
    ax.legend(loc="best", frameon=False)
    fig.tight_layout()
    fig.savefig(out_path)
    plt.close(fig)
    return True


def save_phase_plot(plt, out_path, rows, title):
    rows = sorted_by_float(rows, "iteration")
    x = finite_values(rows, "iteration")
    y = [to_float(row, "learning_phase") for row in rows]
    if not x or not any(math.isfinite(value) for value in y):
        return False

    fig, ax = plt.subplots(figsize=(8.5, 3.8))
    ax.step(x, y, where="post", linewidth=1.8)
    set_common_style(ax, title, "iteration", "learning phase")
    ax.set_yticks(range(len(PHASE_LABELS)))
    ax.set_yticklabels(PHASE_LABELS)
    ax.set_ylim(-0.5, len(PHASE_LABELS) - 0.5)
    fig.tight_layout()
    fig.savefig(out_path)
    plt.close(fig)
    return True


def save_power_efficiency_plot(plt, out_path, rows, title):
    rows = sorted_by_float(rows, "iteration")
    x = finite_values(rows, "iteration")
    if not x:
        return False

    fig, ax1 = plt.subplots(figsize=(8.5, 4.5))
    plotted = False
    for key, label in POWER_METRICS:
        y = [to_float(row, key) for row in rows]
        if any(math.isfinite(value) for value in y):
            ax1.plot(x, y, marker="o", markersize=2.5, linewidth=1.4, label=label)
            plotted = True

    ax2 = ax1.twinx()
    eff = [to_float(row, "efficiency_pct") for row in rows]
    if any(math.isfinite(value) for value in eff):
        ax2.plot(x, eff, color="#2ca02c", linewidth=1.6, label="efficiency")
        ax2.set_ylabel("efficiency (%)")
        plotted = True

    if not plotted:
        plt.close(fig)
        return False

    set_common_style(ax1, title, "iteration", "power (mW)")
    lines1, labels1 = ax1.get_legend_handles_labels()
    lines2, labels2 = ax2.get_legend_handles_labels()
    ax1.legend(lines1 + lines2, labels1 + labels2, loc="best", frameon=False)
    fig.tight_layout()
    fig.savefig(out_path)
    plt.close(fig)
    return True


def save_case_summary_plot(plt, np, out_path, rows, metric, ylabel):
    groups = group_by(rows, "scenario")
    scenarios = sorted(groups)
    cases = [case for case in CASES if any(row.get("case") == case for row in rows)]
    if not scenarios or not cases:
        return False

    x = np.arange(len(scenarios))
    width = min(0.16, 0.8 / max(len(cases), 1))
    fig, ax = plt.subplots(figsize=(11.0, 5.2))
    plotted = False
    for offset, case in enumerate(cases):
        values = []
        for scenario in scenarios:
            row = next(
                (item for item in groups[scenario] if item.get("case") == case),
                None,
            )
            values.append(to_float(row, metric) if row else math.nan)
        if any(math.isfinite(value) for value in values):
            ax.bar(x + (offset - (len(cases) - 1) / 2.0) * width,
                   values, width, label=case)
            plotted = True

    if not plotted:
        plt.close(fig)
        return False

    set_common_style(ax, ylabel, "scenario", ylabel)
    ax.set_xticks(x)
    ax.set_xticklabels(scenarios, rotation=18, ha="right")
    ax.legend(loc="best", frameon=False, ncols=2)
    fig.tight_layout()
    fig.savefig(out_path)
    plt.close(fig)
    return True


def save_paper_comparison_plot(plt, np, out_path, rows):
    rows = [row for row in rows if row.get("metric") in PAPER_METRICS]
    rows = [
        row for row in rows
        if math.isfinite(to_float(row, "paper_value")) and
        math.isfinite(to_float(row, "model_value"))
    ]
    if not rows:
        return False

    labels = [f"{row['metric']}\n{row['scenario']}" for row in rows]
    paper = [to_float(row, "paper_value") for row in rows]
    model = [to_float(row, "model_value") for row in rows]
    x = np.arange(len(rows))
    width = 0.38

    fig, ax = plt.subplots(figsize=(max(10.0, len(rows) * 0.58), 5.8))
    ax.bar(x - width / 2.0, paper, width, label="paper")
    ax.bar(x + width / 2.0, model, width, label="model")
    set_common_style(ax, "Paper comparison", "metric / scenario", "reported value")
    ax.set_xticks(x)
    ax.set_xticklabels(labels, rotation=60, ha="right")
    ax.legend(loc="best", frameon=False)
    fig.tight_layout()
    fig.savefig(out_path)
    plt.close(fig)
    return True


def save_full_droop_reduction_plot(plt, np, out_path, rows):
    rows = [
        row for row in rows
        if row.get("metric") == "droop_reduction" and
        is_sparsity_scenario(row.get("scenario", "")) and
        math.isfinite(to_float(row, "paper_value")) and
        math.isfinite(to_float(row, "model_value"))
    ]
    rows = sorted(rows, key=lambda row: (sparsity_value(row["scenario"]),
                                         row["scenario"]))
    if not rows:
        return False

    labels = [f"{sparsity_value(row['scenario']):g}" for row in rows]
    paper = [to_float(row, "paper_value") for row in rows]
    model = [to_float(row, "model_value") for row in rows]
    x = np.arange(len(rows))
    width = 0.34

    fig, ax = plt.subplots(figsize=(8.8, 4.8))
    paper_bars = ax.bar(x - width / 2.0, paper, width, label="paper",
                        color="#2b78b8")
    model_bars = ax.bar(x + width / 2.0, model, width, label="model full",
                        color="#f28e2b")

    set_common_style(ax, "Full case droop reduction vs paper",
                     "Sparsity (%)", "Droop reduction (%)")
    ax.set_xticks(x)
    ax.set_xticklabels(labels)
    values = paper + model
    y_min = min(values)
    y_max = max(values)
    y_span = max(y_max - y_min, 1.0)
    ax.set_ylim(min(0.0, y_min - 0.15 * y_span),
                y_max + 0.18 * y_span)
    ax.legend(loc="best", frameon=False)

    for bars in (paper_bars, model_bars):
        for bar in bars:
            height = bar.get_height()
            label_pad = 0.03 * y_span
            ax.text(
                bar.get_x() + bar.get_width() / 2.0,
                height + label_pad if height >= 0.0 else height - label_pad,
                f"{height:.1f}%",
                ha="center",
                va="bottom" if height >= 0.0 else "top",
                fontsize=8,
            )

    fig.tight_layout()
    fig.savefig(out_path)
    plt.close(fig)
    return True


def save_performance_plot(plt, np, out_path, rows):
    rows = [
        row for row in rows
        if row.get("scenario") != "overall_ai" and
        math.isfinite(to_float(row, "performance_improvement_pct"))
    ]
    if not rows:
        return False

    labels = [f"{row['comparison']}\n{row['scenario']}" for row in rows]
    values = [to_float(row, "performance_improvement_pct") for row in rows]
    x = np.arange(len(rows))

    fig, ax = plt.subplots(figsize=(max(10.0, len(rows) * 0.5), 5.2))
    colors = ["#2ca02c" if value >= 0.0 else "#d62728" for value in values]
    ax.bar(x, values, color=colors)
    ax.axhline(0.0, color="black", linewidth=0.8)
    set_common_style(ax, "Performance estimate", "comparison / scenario",
                     "performance improvement (%)")
    ax.set_xticks(x)
    ax.set_xticklabels(labels, rotation=60, ha="right")
    fig.tight_layout()
    fig.savefig(out_path)
    plt.close(fig)
    return True


def performance_score(row, veff_key):
    if not row:
        return math.nan
    veff_v = to_float(row, veff_key)
    vth_v = to_float(row, "vth_v")
    alpha = to_float(row, "alpha")
    if not all(math.isfinite(value) for value in (veff_v, vth_v, alpha)):
        return math.nan
    if veff_v <= vth_v or veff_v <= 0.0:
        return 0.0
    return 100.0 * ((veff_v - vth_v) ** alpha) / veff_v


def pct_gain(new_value, old_value):
    if not math.isfinite(new_value) or not math.isfinite(old_value):
        return math.nan
    if abs(old_value) <= 1.0e-18:
        return math.nan
    return (new_value / old_value - 1.0) * 100.0


def format_pct(value):
    if not math.isfinite(value):
        return ""
    return f"+{value:.1f}%" if value >= 0.0 else f"{value:.1f}%"


def annotate_gain(ax, base_x, base_y, full_x, full_y, gain_pct, y_pad):
    if not all(math.isfinite(value) for value in
               (base_x, base_y, full_x, full_y, gain_pct)):
        return
    top = max(base_y, full_y)
    ax.annotate(
        "",
        xy=(full_x, full_y),
        xytext=(base_x, base_y),
        arrowprops={
            "arrowstyle": "->",
            "color": "#c6534b",
            "linestyle": "--",
            "linewidth": 1.1,
            "shrinkA": 2,
            "shrinkB": 2,
        },
    )
    ax.text(
        (base_x + full_x) / 2.0,
        top + y_pad,
        format_pct(gain_pct),
        ha="center",
        va="bottom",
        fontsize=9,
        fontweight="bold",
    )


def save_overall_benefits_plot(plt, np, out_path, case_rows, energy_rows,
                               performance_rows):
    summary = {
        (row.get("case"), row.get("scenario")): row
        for row in case_rows
    }
    scenarios = sorted(
        {
            row.get("scenario")
            for row in case_rows
            if is_sparsity_scenario(row.get("scenario", ""))
        },
        key=lambda scenario: (sparsity_value(scenario), scenario),
    )
    if not scenarios:
        return False

    x = np.arange(len(scenarios))
    width = 0.22
    offsets = {
        case: (index - (len(BENEFIT_CASES) - 1) / 2.0) * width
        for index, (case, _, _, _) in enumerate(BENEFIT_CASES)
    }

    perf_lookup = {
        (row.get("comparison"), row.get("scenario")): row
        for row in performance_rows
    }
    energy_lookup = {
        (row.get("comparison"), row.get("scenario")): row
        for row in energy_rows
    }

    tops_per_w_values = {case: [] for case, _, _, _ in BENEFIT_CASES}
    perf_values = {case: [] for case, _, _, _ in BENEFIT_CASES}

    for scenario in scenarios:
        full_row = (
            perf_lookup.get(("full_vs_no_learning", scenario)) or
            perf_lookup.get(("full_vs_no_eec", scenario))
        )
        no_learning_row = perf_lookup.get(("full_vs_no_learning", scenario))
        no_eec_row = perf_lookup.get(("full_vs_no_eec", scenario))

        tops_per_w_values["no_eec"].append(
            to_float(no_eec_row, "baseline_tops_per_w"))
        tops_per_w_values["no_learning"].append(
            to_float(no_learning_row, "baseline_tops_per_w"))
        tops_per_w_values["full"].append(
            to_float(full_row, "full_tops_per_w"))

        perf_values["no_eec"].append(to_float(no_eec_row, "baseline_tops"))
        perf_values["no_learning"].append(
            to_float(no_learning_row, "baseline_tops"))
        perf_values["full"].append(to_float(full_row, "full_tops"))

        if not math.isfinite(perf_values["no_eec"][-1]):
            perf_values["no_eec"][-1] = performance_score(
                no_eec_row, "baseline_veff_v")
        if not math.isfinite(perf_values["no_learning"][-1]):
            perf_values["no_learning"][-1] = performance_score(
                no_learning_row, "baseline_veff_v")
        if not math.isfinite(perf_values["full"][-1]):
            perf_values["full"][-1] = performance_score(
                full_row, "full_veff_v")

    eff_finite = [
        value for values in tops_per_w_values.values() for value in values
        if math.isfinite(value)
    ]
    perf_finite = [
        value for values in perf_values.values() for value in values
        if math.isfinite(value)
    ]
    if not eff_finite and not perf_finite:
        return False

    fig, (ax_eff, ax_perf) = plt.subplots(1, 2, figsize=(12.8, 4.35))
    fig.suptitle("Overall Benefits for AI Computing", fontsize=18,
                 fontweight="bold", y=0.98)

    for case, label, face_color, edge_color in BENEFIT_CASES:
        positions = x + offsets[case]
        ax_eff.bar(positions, tops_per_w_values[case], width, label=label,
                   color=face_color, edgecolor=edge_color, linewidth=1.4)
        ax_perf.bar(positions, perf_values[case], width, label=label,
                    color=face_color, edgecolor=edge_color, linewidth=1.4)

    sparsity_labels = [
        f"{sparsity_value(scenario):g}" for scenario in scenarios
    ]
    for ax in (ax_eff, ax_perf):
        ax.set_xticks(x)
        ax.set_xticklabels(sparsity_labels, fontsize=11, fontweight="bold")
        ax.set_xlabel("Sparsity (%)", fontsize=12, fontweight="bold")
        ax.grid(True, axis="y", alpha=0.3)
        ax.set_axisbelow(True)

    ax_eff.set_title("Efficiency Improvement", fontsize=13,
                     fontweight="bold")
    ax_eff.set_ylabel("Eff. (TOPS/W)", fontsize=12, fontweight="bold")
    ax_perf.set_title("Perf. Improvement", fontsize=13, fontweight="bold")
    ax_perf.set_ylabel("Perf. (TOPS)", fontsize=12, fontweight="bold")

    eff_pad = max(max(eff_finite) * 0.035, 1.0) if eff_finite else 1.0
    perf_pad = max(max(perf_finite) * 0.04, 1.0) if perf_finite else 1.0
    for index, scenario in enumerate(scenarios):
        eff_gain = pct_gain(tops_per_w_values["full"][index],
                            tops_per_w_values["no_learning"][index])
        if not math.isfinite(eff_gain):
            energy_row = energy_lookup.get(("full_vs_no_learning", scenario), {})
            eff_gain = to_float(energy_row, "energy_saving_pct")
        annotate_gain(
            ax_eff,
            x[index] + offsets["no_learning"],
            tops_per_w_values["no_learning"][index],
            x[index] + offsets["full"],
            tops_per_w_values["full"][index],
            eff_gain,
            eff_pad,
        )

        perf_row = perf_lookup.get(("full_vs_no_learning", scenario), {})
        annotate_gain(
            ax_perf,
            x[index] + offsets["no_learning"],
            perf_values["no_learning"][index],
            x[index] + offsets["full"],
            perf_values["full"][index],
            to_float(perf_row, "performance_improvement_pct"),
            perf_pad,
        )

    if eff_finite:
        ax_eff.set_ylim(0.0, max(eff_finite) + eff_pad * 4.0)
    if perf_finite:
        ax_perf.set_ylim(0.0, max(perf_finite) + perf_pad * 4.0)

    handles, labels = ax_eff.get_legend_handles_labels()
    fig.legend(handles, labels, loc="upper center", bbox_to_anchor=(0.46, 0.91),
               ncols=3, frameon=False, fontsize=11)
    fig.text(
        0.99,
        0.02,
        "TOPS uses the calibrated alpha-power droop/overshoot model.",
        ha="right",
        fontsize=8,
    )
    fig.tight_layout(rect=(0.0, 0.05, 1.0, 0.85))
    fig.savefig(out_path)
    plt.close(fig)
    return True


def add_gallery_item(gallery, title, path, out_dir):
    gallery.append((title, path.relative_to(out_dir).as_posix()))


def write_index(path, gallery):
    lines = [
        "<!doctype html>",
        "<html>",
        "<head>",
        "  <meta charset=\"utf-8\">",
        "  <title>PDN Results Visualization</title>",
        "  <style>",
        "    body { font-family: Arial, sans-serif; margin: 24px; color: #222; }",
        "    h1 { font-size: 24px; margin-bottom: 8px; }",
        "    h2 { font-size: 18px; margin-top: 28px; }",
        "    figure { margin: 18px 0 28px; }",
        "    img { max-width: 100%; border: 1px solid #ddd; }",
        "    figcaption { margin-top: 6px; color: #555; }",
        "  </style>",
        "</head>",
        "<body>",
        "  <h1>PDN Results Visualization</h1>",
        "  <p>Generated from CSV outputs under the selected results directory.</p>",
    ]
    for title, rel_path in gallery:
        lines.extend([
            "  <figure>",
            f"    <img src=\"{rel_path}\" alt=\"{title}\">",
            f"    <figcaption>{title}</figcaption>",
            "  </figure>",
        ])
    lines.extend(["</body>", "</html>", ""])
    path.write_text("\n".join(lines), encoding="utf-8")


def visualize(results_dir, out_dir, image_format, dpi):
    plt, np = import_matplotlib()
    plt.rcParams.update({
        "figure.dpi": dpi,
        "savefig.dpi": dpi,
        "axes.spines.top": False,
        "axes.spines.right": False,
    })

    out_dir.mkdir(parents=True, exist_ok=True)
    gallery = []

    for case in CASES:
        rows = read_rows(results_dir / case / "pdn_learning_metrics.csv")
        if not rows:
            continue
        for scenario, scenario_rows in sorted(group_by(rows, "scenario").items()):
            base_name = f"{slug(case)}_{slug(scenario)}"

            path = out_dir / f"{base_name}_voltage.{image_format}"
            if save_line_plot(
                plt, path, scenario_rows, "iteration", VOLTAGE_METRICS,
                f"{case} / {scenario}: voltage window metrics", "mV",
            ):
                add_gallery_item(gallery, f"{case} / {scenario}: voltage", path, out_dir)

            path = out_dir / f"{base_name}_power_efficiency.{image_format}"
            if save_power_efficiency_plot(
                plt, path, scenario_rows,
                f"{case} / {scenario}: power and efficiency",
            ):
                add_gallery_item(gallery, f"{case} / {scenario}: power", path, out_dir)

            path = out_dir / f"{base_name}_aux_power.{image_format}"
            if save_line_plot(
                plt, path, scenario_rows, "iteration", AUX_POWER_METRICS,
                f"{case} / {scenario}: auxiliary power", "mW",
            ):
                add_gallery_item(gallery, f"{case} / {scenario}: auxiliary power",
                                 path, out_dir)

            path = out_dir / f"{base_name}_current_balance.{image_format}"
            if save_line_plot(
                plt, path, scenario_rows, "iteration",
                (("current_stddev_ma", "avg sigma"),
                 ("current_stddev_min_ma", "min sigma")),
                f"{case} / {scenario}: source current sharing", "mA",
            ):
                add_gallery_item(gallery, f"{case} / {scenario}: current sharing",
                                 path, out_dir)

            path = out_dir / f"{base_name}_learning_controls.{image_format}"
            if save_line_plot(
                plt, path, scenario_rows, "iteration", LEARNING_METRICS,
                f"{case} / {scenario}: learned voltage controls", "mV",
            ):
                add_gallery_item(gallery, f"{case} / {scenario}: learning controls",
                                 path, out_dir)

            path = out_dir / f"{base_name}_learning_phase.{image_format}"
            if save_phase_plot(
                plt, path, scenario_rows,
                f"{case} / {scenario}: learning phase",
            ):
                add_gallery_item(gallery, f"{case} / {scenario}: learning phase",
                                 path, out_dir)

    summary_rows = read_rows(results_dir / "case_summary.csv")
    for metric, ylabel in CASE_SUMMARY_METRICS:
        path = out_dir / f"case_summary_{slug(metric)}.{image_format}"
        if save_case_summary_plot(plt, np, path, summary_rows, metric, ylabel):
            add_gallery_item(gallery, f"Case summary: {ylabel}", path, out_dir)

    paper_rows = read_rows(results_dir / "paper_comparison_completed.csv")
    if not paper_rows:
        paper_rows = read_rows(results_dir / "full" / "pdn_paper_comparison.csv")
    path = out_dir / f"paper_comparison.{image_format}"
    if save_paper_comparison_plot(plt, np, path, paper_rows):
        add_gallery_item(gallery, "Paper value vs model value", path, out_dir)

    path = out_dir / f"full_droop_reduction_vs_paper.{image_format}"
    if save_full_droop_reduction_plot(plt, np, path, paper_rows):
        add_gallery_item(gallery, "Full case droop reduction vs paper",
                         path, out_dir)

    perf_rows = read_rows(results_dir / "performance_summary.csv")
    path = out_dir / f"performance_summary.{image_format}"
    if save_performance_plot(plt, np, path, perf_rows):
        add_gallery_item(gallery, "Performance improvement estimates", path, out_dir)

    energy_rows = read_rows(results_dir / "energy_summary.csv")
    path = out_dir / f"overall_ai_benefits.{image_format}"
    if save_overall_benefits_plot(
        plt, np, path, summary_rows, energy_rows, perf_rows,
    ):
        add_gallery_item(gallery, "Overall benefits for AI computing",
                         path, out_dir)

    write_index(out_dir / "index.html", gallery)
    return gallery


def main():
    parser = argparse.ArgumentParser(
        description="Generate PDN visualization figures from experiment CSVs.")
    parser.add_argument("--results-dir", type=Path, default=Path("results"),
                        help="Directory containing case result folders.")
    parser.add_argument("--out-dir", type=Path, default=None,
                        help="Output directory for figures. Default: results/figures.")
    parser.add_argument("--format", choices=("png", "pdf", "svg"), default="png",
                        help="Figure format.")
    parser.add_argument("--dpi", type=int, default=180,
                        help="Raster output DPI.")
    args = parser.parse_args()

    out_dir = args.out_dir or args.results_dir / "figures"
    gallery = visualize(args.results_dir, out_dir, args.format, args.dpi)
    print(f"Wrote {len(gallery)} figures to {out_dir}")
    print(f"Open {out_dir / 'index.html'} to browse the visualization gallery")


if __name__ == "__main__":
    main()
