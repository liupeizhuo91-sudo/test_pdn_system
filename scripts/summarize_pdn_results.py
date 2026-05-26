#!/usr/bin/env python3
"""Summarize automated PDN experiment runs.

The script expects result folders produced by build/Makefile:

  results/full/pdn_learning_metrics.csv
  results/no_learning/pdn_learning_metrics.csv
  results/no_eec/pdn_learning_metrics.csv
  results/no_balance/pdn_learning_metrics.csv
  results/baseline_all_off/pdn_learning_metrics.csv

It writes:

  results/case_summary.csv
  results/energy_summary.csv
  results/paper_comparison_completed.csv
  results/performance_summary.csv

Performance is estimated with an alpha-power DVFS model:

  Veff = Vnom - droop
  fmax_score = (Veff - Vth)^alpha / Veff
  performance_improvement = (fmax_score_full / fmax_score_baseline - 1) * 100
"""

import argparse
import csv
import math
from pathlib import Path


AI_SCENARIOS = (
    "dense_10pct_75pct",
    "medium_50pct_50pct",
    "sparse_90pct_25pct",
)

CASES = (
    "full",
    "no_learning",
    "no_eec",
    "no_balance",
    "baseline_all_off",
)

COMPARISONS = (
    ("full_vs_no_learning", "no_learning"),
    ("full_vs_no_eec", "no_eec"),
    ("full_vs_no_balance", "no_balance"),
    ("full_vs_baseline_all_off", "baseline_all_off"),
)

PAPER_COMPLETION_COMPARISONS = (
    "full_vs_no_learning",
    "full_vs_no_eec",
    "full_vs_baseline_all_off",
)

DEFAULT_VNOM_V = 0.8
DEFAULT_VTH_V = 0.35
DEFAULT_ALPHA = 1.3
DEFAULT_DROOP_PERCENTILE = 95.0
DEFAULT_OVERSHOOT_ALLOWANCE_MV = 10.0
DEFAULT_OVERSHOOT_PENALTY = 1.0


def read_rows(path):
    if not path.exists():
        raise FileNotFoundError(f"missing required CSV: {path}")
    with path.open(newline="") as f:
        return list(csv.DictReader(f))


def write_rows(path, fieldnames, rows):
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)


def to_float(row, key):
    value = row.get(key, "")
    if value == "":
        return math.nan
    return float(value)


def avg(values):
    values = [v for v in values if math.isfinite(v)]
    return sum(values) / len(values) if values else math.nan


def percentile(values, pct):
    values = sorted(v for v in values if math.isfinite(v))
    if not values:
        return math.nan
    if len(values) == 1:
        return values[0]

    pct = max(0.0, min(100.0, pct))
    pos = (pct / 100.0) * (len(values) - 1)
    lo = int(math.floor(pos))
    hi = int(math.ceil(pos))
    if lo == hi:
        return values[lo]
    frac = pos - lo
    return values[lo] * (1.0 - frac) + values[hi] * frac


def fmt(value):
    return "" if not math.isfinite(value) else f"{value:.6f}"


def csv_flag(row, key):
    value = row.get(key, "")
    return value not in ("", "0", "0.000000", "false", "False")


def load_case(results_dir, case):
    return read_rows(results_dir / case / "pdn_learning_metrics.csv")


def scenario_rows(rows, scenario):
    return [row for row in rows if row["scenario"] == scenario]


def summarize_case(case, rows):
    out = []
    scenarios = sorted({row["scenario"] for row in rows})
    for scenario in scenarios:
        subset = scenario_rows(rows, scenario)
        out.append({
            "case": case,
            "scenario": scenario,
            "windows": len(subset),
            "avg_pin_mw": fmt(avg(to_float(r, "pin_mw") for r in subset)),
            "avg_pout_mw": fmt(avg(to_float(r, "pout_mw") for r in subset)),
            "avg_efficiency_pct": fmt(avg(to_float(r, "efficiency_pct") for r in subset)),
            "avg_control_power_mw": fmt(avg(to_float(r, "control_power_mw") for r in subset)),
            "avg_guardband_power_mw": fmt(avg(to_float(r, "guardband_power_mw") for r in subset)),
            "avg_switching_power_mw": fmt(avg(to_float(r, "switching_power_mw") for r in subset)),
            "rail_violation_windows": sum(1 for r in subset if csv_flag(r, "rail_violation")),
            "best_droop_mv": fmt(min(to_float(r, "droop_mv") for r in subset)),
            "best_pkpk_mv": fmt(min(to_float(r, "pkpk_mv") for r in subset)),
            "avg_load_ma": fmt(avg(to_float(r, "avg_load_ma") for r in subset)),
        })
    return out


def scenario_power(rows, scenario):
    if scenario == "overall_ai":
        subset = [row for row in rows if row["scenario"] in AI_SCENARIOS]
    else:
        subset = scenario_rows(rows, scenario)
    return {
        "pin_mw": avg(to_float(r, "pin_mw") for r in subset),
        "pout_mw": avg(to_float(r, "pout_mw") for r in subset),
        "efficiency_pct": avg(to_float(r, "efficiency_pct") for r in subset),
    }


def energy_row(name, scenario, full_rows, base_rows):
    full = scenario_power(full_rows, scenario)
    base = scenario_power(base_rows, scenario)
    saving = (
        (base["pin_mw"] - full["pin_mw"]) / base["pin_mw"] * 100.0
        if math.isfinite(base["pin_mw"]) and abs(base["pin_mw"]) > 1.0e-18
        else math.nan
    )
    return {
        "comparison": name,
        "scenario": scenario,
        "full_pin_mw": fmt(full["pin_mw"]),
        "baseline_pin_mw": fmt(base["pin_mw"]),
        "energy_saving_pct": fmt(saving),
        "full_pout_mw": fmt(full["pout_mw"]),
        "baseline_pout_mw": fmt(base["pout_mw"]),
        "full_efficiency_pct": fmt(full["efficiency_pct"]),
        "baseline_efficiency_pct": fmt(base["efficiency_pct"]),
    }


def build_energy_summary(case_rows):
    full_rows = case_rows["full"]
    rows = []
    for name, baseline_case in COMPARISONS:
        base_rows = case_rows[baseline_case]
        for scenario in AI_SCENARIOS:
            rows.append(energy_row(name, scenario, full_rows, base_rows))

        rows.append(energy_row(name, "overall_ai", full_rows, base_rows))
    return rows


def droop_stat(rows, scenario, pct):
    if scenario == "overall_ai":
        subset = [row for row in rows if row["scenario"] in AI_SCENARIOS]
    else:
        subset = scenario_rows(rows, scenario)
    return percentile((to_float(row, "droop_mv") for row in subset), pct)


def overshoot_stat(rows, scenario, pct):
    if scenario == "overall_ai":
        subset = [row for row in rows if row["scenario"] in AI_SCENARIOS]
    else:
        subset = scenario_rows(rows, scenario)
    return percentile((to_float(row, "overshoot_mv") for row in subset), pct)


def voltage_limiter_mv(droop_mv, overshoot_mv, overshoot_allowance_mv,
                       overshoot_penalty):
    if not math.isfinite(droop_mv) or not math.isfinite(overshoot_mv):
        return math.nan
    overshoot_excess = max(0.0, overshoot_mv - overshoot_allowance_mv)
    return droop_mv + overshoot_penalty * overshoot_excess


def scenario_weight(rows, scenario):
    subset = scenario_rows(rows, scenario)
    return avg(to_float(row, "avg_load_ma") for row in subset)


def fmax_score(veff_v, vth_v, alpha):
    if not math.isfinite(veff_v) or veff_v <= vth_v or veff_v <= 0.0:
        return math.nan
    return ((veff_v - vth_v) ** alpha) / veff_v


def performance_row(name, scenario, full_rows, base_rows, vnom_v, vth_v,
                    alpha, droop_percentile, overshoot_allowance_mv,
                    overshoot_penalty):
    full_droop_mv = droop_stat(full_rows, scenario, droop_percentile)
    base_droop_mv = droop_stat(base_rows, scenario, droop_percentile)
    full_overshoot_mv = overshoot_stat(full_rows, scenario, droop_percentile)
    base_overshoot_mv = overshoot_stat(base_rows, scenario, droop_percentile)
    full_limiter_mv = voltage_limiter_mv(
        full_droop_mv, full_overshoot_mv, overshoot_allowance_mv,
        overshoot_penalty)
    base_limiter_mv = voltage_limiter_mv(
        base_droop_mv, base_overshoot_mv, overshoot_allowance_mv,
        overshoot_penalty)
    full_veff_v = vnom_v - full_limiter_mv * 1.0e-3
    base_veff_v = vnom_v - base_limiter_mv * 1.0e-3
    full_score = fmax_score(full_veff_v, vth_v, alpha)
    base_score = fmax_score(base_veff_v, vth_v, alpha)
    f_ratio = (
        full_score / base_score
        if math.isfinite(full_score) and math.isfinite(base_score) and
        abs(base_score) > 1.0e-18
        else math.nan
    )
    improvement = (f_ratio - 1.0) * 100.0 if math.isfinite(f_ratio) else math.nan

    return {
        "comparison": name,
        "scenario": scenario,
        "droop_stat": f"p{droop_percentile:g}",
        "full_droop_mv": fmt(full_droop_mv),
        "baseline_droop_mv": fmt(base_droop_mv),
        "full_overshoot_mv": fmt(full_overshoot_mv),
        "baseline_overshoot_mv": fmt(base_overshoot_mv),
        "full_limiter_mv": fmt(full_limiter_mv),
        "baseline_limiter_mv": fmt(base_limiter_mv),
        "full_veff_v": fmt(full_veff_v),
        "baseline_veff_v": fmt(base_veff_v),
        "vnom_v": fmt(vnom_v),
        "vth_v": fmt(vth_v),
        "alpha": fmt(alpha),
        "overshoot_allowance_mv": fmt(overshoot_allowance_mv),
        "overshoot_penalty": fmt(overshoot_penalty),
        "f_ratio": fmt(f_ratio),
        "performance_improvement_pct": fmt(improvement),
        "weight": fmt(scenario_weight(full_rows, scenario)
                      if scenario != "overall_ai" else math.nan),
        "status": "estimated" if math.isfinite(improvement) else "invalid",
        "note": "alpha-power estimate using droop plus excess-overshoot limiter",
    }


def overall_performance_row(name, scenario_rows_, vnom_v, vth_v, alpha,
                            droop_percentile, overshoot_allowance_mv,
                            overshoot_penalty):
    weighted = []
    for row in scenario_rows_:
        f_ratio = float(row["f_ratio"]) if row["f_ratio"] else math.nan
        weight = float(row["weight"]) if row["weight"] else math.nan
        if math.isfinite(f_ratio) and f_ratio > 0.0 and math.isfinite(weight) and weight > 0.0:
            weighted.append((weight, f_ratio))

    if weighted:
        total_weight = sum(weight for weight, _ in weighted)
        harmonic_speedup = total_weight / sum(weight / f_ratio for weight, f_ratio in weighted)
        improvement = (harmonic_speedup - 1.0) * 100.0
    else:
        total_weight = math.nan
        harmonic_speedup = math.nan
        improvement = math.nan
    complete = len(weighted) == len(scenario_rows_)

    return {
        "comparison": name,
        "scenario": "overall_ai",
        "droop_stat": f"p{droop_percentile:g}",
        "full_droop_mv": "",
        "baseline_droop_mv": "",
        "full_overshoot_mv": "",
        "baseline_overshoot_mv": "",
        "full_limiter_mv": "",
        "baseline_limiter_mv": "",
        "full_veff_v": "",
        "baseline_veff_v": "",
        "vnom_v": fmt(vnom_v),
        "vth_v": fmt(vth_v),
        "alpha": fmt(alpha),
        "overshoot_allowance_mv": fmt(overshoot_allowance_mv),
        "overshoot_penalty": fmt(overshoot_penalty),
        "f_ratio": fmt(harmonic_speedup),
        "performance_improvement_pct": fmt(improvement),
        "weight": fmt(total_weight),
        "status": "estimated" if math.isfinite(improvement) and complete else "invalid",
        "note": "load-current weighted harmonic mean of scenario f-ratios"
                if complete else "invalid because at least one scenario f-ratio is unavailable",
    }


def build_performance_summary(case_rows, vnom_v, vth_v, alpha,
                              droop_percentile, overshoot_allowance_mv,
                              overshoot_penalty):
    full_rows = case_rows["full"]
    rows = []
    for name, baseline_case in COMPARISONS:
        base_rows = case_rows[baseline_case]
        comparison_rows = []
        for scenario in AI_SCENARIOS:
            row = performance_row(name, scenario, full_rows, base_rows,
                                  vnom_v, vth_v, alpha, droop_percentile,
                                  overshoot_allowance_mv, overshoot_penalty)
            comparison_rows.append(row)
            rows.append(row)
        rows.append(overall_performance_row(name, comparison_rows, vnom_v,
                                            vth_v, alpha, droop_percentile,
                                            overshoot_allowance_mv,
                                            overshoot_penalty))
    return rows


def complete_paper_comparison(results_dir, energy_rows, performance_rows):
    source = results_dir / "full" / "pdn_paper_comparison.csv"
    if not source.exists():
        return []

    rows = read_rows(source)
    energy_values = [
        float(row["energy_saving_pct"])
        for row in energy_rows
        if row["scenario"] == "overall_ai" and
        row["comparison"] in PAPER_COMPLETION_COMPARISONS and
        row["energy_saving_pct"] != ""
    ]
    if energy_values:
        energy_min = min(energy_values)
        energy_max = max(energy_values)
        for row in rows:
            if row["metric"] == "energy_saving_min":
                row["model_value"] = fmt(energy_min)
                row["note"] = "Computed from automated paired runs"
            elif row["metric"] == "energy_saving_max":
                row["model_value"] = fmt(energy_max)
                row["note"] = "Computed from automated paired runs"

    performance_values = [
        float(row["performance_improvement_pct"])
        for row in performance_rows
        if row["scenario"] == "overall_ai" and
        row["comparison"] in PAPER_COMPLETION_COMPARISONS and
        row["status"] == "estimated" and
        row["performance_improvement_pct"] != ""
    ]
    if performance_values:
        performance_min = min(performance_values)
        performance_max = max(performance_values)
        for row in rows:
            if row["metric"] == "performance_improvement_min":
                row["model_value"] = fmt(performance_min)
                row["note"] = "Estimated by alpha-power DVFS model from automated paired runs"
            elif row["metric"] == "performance_improvement_max":
                row["model_value"] = fmt(performance_max)
                row["note"] = "Estimated by alpha-power DVFS model from automated paired runs"
    return rows


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--results-dir", default="results")
    parser.add_argument("--vnom-v", type=float, default=DEFAULT_VNOM_V,
                        help="Nominal supply voltage for the performance model.")
    parser.add_argument("--vth-v", type=float, default=DEFAULT_VTH_V,
                        help="Effective threshold voltage for alpha-power law.")
    parser.add_argument("--alpha", type=float, default=DEFAULT_ALPHA,
                        help="Alpha exponent for alpha-power law.")
    parser.add_argument("--droop-percentile", type=float,
                        default=DEFAULT_DROOP_PERCENTILE,
                        help="Droop percentile used as performance limiter.")
    parser.add_argument("--overshoot-allowance-mv", type=float,
                        default=DEFAULT_OVERSHOOT_ALLOWANCE_MV,
                        help="Overshoot allowed before performance is penalized.")
    parser.add_argument("--overshoot-penalty", type=float,
                        default=DEFAULT_OVERSHOOT_PENALTY,
                        help="Penalty multiplier for overshoot above allowance.")
    args = parser.parse_args()

    results_dir = Path(args.results_dir)
    case_rows = {case: load_case(results_dir, case) for case in CASES}

    case_summary = []
    for case, rows in case_rows.items():
        case_summary.extend(summarize_case(case, rows))
    write_rows(
        results_dir / "case_summary.csv",
        (
            "case",
            "scenario",
            "windows",
            "avg_pin_mw",
            "avg_pout_mw",
            "avg_efficiency_pct",
            "avg_control_power_mw",
            "avg_guardband_power_mw",
            "avg_switching_power_mw",
            "rail_violation_windows",
            "best_droop_mv",
            "best_pkpk_mv",
            "avg_load_ma",
        ),
        case_summary,
    )

    energy_summary = build_energy_summary(case_rows)
    write_rows(
        results_dir / "energy_summary.csv",
        (
            "comparison",
            "scenario",
            "full_pin_mw",
            "baseline_pin_mw",
            "energy_saving_pct",
            "full_pout_mw",
            "baseline_pout_mw",
            "full_efficiency_pct",
            "baseline_efficiency_pct",
        ),
        energy_summary,
    )

    performance_summary = build_performance_summary(
        case_rows, args.vnom_v, args.vth_v, args.alpha,
        args.droop_percentile, args.overshoot_allowance_mv,
        args.overshoot_penalty)
    write_rows(
        results_dir / "performance_summary.csv",
        (
            "comparison",
            "scenario",
            "droop_stat",
            "full_droop_mv",
            "baseline_droop_mv",
            "full_overshoot_mv",
            "baseline_overshoot_mv",
            "full_limiter_mv",
            "baseline_limiter_mv",
            "full_veff_v",
            "baseline_veff_v",
            "vnom_v",
            "vth_v",
            "alpha",
            "overshoot_allowance_mv",
            "overshoot_penalty",
            "f_ratio",
            "performance_improvement_pct",
            "weight",
            "status",
            "note",
        ),
        performance_summary,
    )

    completed = complete_paper_comparison(
        results_dir, energy_summary, performance_summary)
    if completed:
        write_rows(
            results_dir / "paper_comparison_completed.csv",
            ("metric", "scenario", "paper_value", "model_value", "unit", "note"),
            completed,
        )

    print(f"Wrote summaries to {results_dir}")


if __name__ == "__main__":
    main()
