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

Window selection defaults to the simulation-selected best peak-to-peak window.
The paper-reported best iteration is kept only as a reference value.
"""

import argparse
import csv
import math
from pathlib import Path


AI_SCENARIOS = (
    "dense_10pct_75pct",
    "dense_mid_30pct_62p5pct",
    "medium_50pct_50pct",
    "sparse_mid_70pct_37p5pct",
    "sparse_90pct_25pct",
)

PAPER_BEST_ITER = {
    "dense_10pct_75pct": 29,
    "dense_mid_30pct_62p5pct": 26,
    "medium_50pct_50pct": 23,
    "sparse_mid_70pct_37p5pct": 23,
    "sparse_90pct_25pct": 23,
}

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
DEFAULT_NOMINAL_TOPS = 4.2
DEFAULT_WINDOW_POLICY = "sim_best"
DEFAULT_STABLE_WINDOWS = 4


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


def pct_reduction(baseline, improved):
    return (
        (baseline - improved) / baseline * 100.0
        if math.isfinite(baseline) and math.isfinite(improved) and
        abs(baseline) > 1.0e-18
        else math.nan
    )


def csv_flag(row, key):
    value = row.get(key, "")
    return value not in ("", "0", "0.000000", "false", "False")


def load_case(results_dir, case):
    return read_rows(results_dir / case / "pdn_learning_metrics.csv")


def scenario_rows(rows, scenario):
    return [row for row in rows if row["scenario"] == scenario]


def sorted_by_float(rows, key):
    return sorted(rows, key=lambda row: to_float(row, key))


def last_rows(rows, count=DEFAULT_STABLE_WINDOWS):
    rows = sorted_by_float(rows, "iteration")
    return rows[-count:] if len(rows) > count else rows


def closest_iteration_row(rows, iteration):
    rows = sorted_by_float(rows, "iteration")
    if not rows:
        return None
    return min(rows, key=lambda row: abs(to_float(row, "iteration") - iteration))


def best_pkpk_from_subset(rows):
    if not rows:
        return None
    return min(
        rows,
        key=lambda row: (
            to_float(row, "pkpk_mv")
            if math.isfinite(to_float(row, "pkpk_mv"))
            else math.inf
        ),
    )


def selected_rows(rows, scenario, policy=DEFAULT_WINDOW_POLICY):
    if scenario == "overall_ai":
        selected = []
        for item in AI_SCENARIOS:
            selected.extend(selected_rows(rows, item, policy))
        return selected

    subset = sorted_by_float(scenario_rows(rows, scenario), "iteration")
    if not subset:
        return []

    if policy == "all":
        return subset

    if policy == "sim_best":
        row = best_pkpk_from_subset(subset)
        return [row] if row else []

    if policy == "paper_iter":
        target = PAPER_BEST_ITER.get(scenario)
        if target is not None:
            row = closest_iteration_row(subset, target)
            return [row] if row else []
        return last_rows(subset)

    if policy == "converged_done":
        done = [row for row in subset if row.get("learning_phase_name") == "done"]
        return last_rows(done or subset)

    if policy == "final_stable":
        return last_rows(subset)

    raise ValueError(f"unknown window policy: {policy}")


def best_pkpk_row(rows, scenario, policy=DEFAULT_WINDOW_POLICY):
    subset = selected_rows(rows, scenario, policy)
    return best_pkpk_from_subset(subset)


def summarize_case(case, rows, window_policy=DEFAULT_WINDOW_POLICY):
    out = []
    scenarios = sorted({row["scenario"] for row in rows})
    for scenario in scenarios:
        all_subset = sorted_by_float(scenario_rows(rows, scenario), "iteration")
        subset = selected_rows(rows, scenario, window_policy)
        best_row = best_pkpk_row(rows, scenario, window_policy)
        if not all_subset or not subset or not best_row:
            continue
        baseline_droop = to_float(all_subset[0], "baseline_droop_mv")
        droop_at_best_pkpk = to_float(best_row, "droop_mv")
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
            "best_pkpk_iteration": fmt(to_float(best_row, "iteration")),
            "baseline_droop_mv": fmt(baseline_droop),
            "droop_at_best_pkpk_mv": fmt(droop_at_best_pkpk),
            "droop_reduction_at_best_pkpk_pct": fmt(
                pct_reduction(baseline_droop, droop_at_best_pkpk)),
            "avg_load_ma": fmt(avg(to_float(r, "avg_load_ma") for r in subset)),
        })
    return out


def scenario_power(rows, scenario, window_policy=DEFAULT_WINDOW_POLICY):
    subset = selected_rows(rows, scenario, window_policy)
    return {
        "pin_mw": avg(to_float(r, "pin_mw") for r in subset),
        "pout_mw": avg(to_float(r, "pout_mw") for r in subset),
        "efficiency_pct": avg(to_float(r, "efficiency_pct") for r in subset),
    }


def energy_row(name, scenario, full_rows, base_rows,
               window_policy=DEFAULT_WINDOW_POLICY):
    full = scenario_power(full_rows, scenario, window_policy)
    base = scenario_power(base_rows, scenario, window_policy)
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


def build_energy_summary(case_rows, window_policy=DEFAULT_WINDOW_POLICY):
    full_rows = case_rows["full"]
    rows = []
    for name, baseline_case in COMPARISONS:
        base_rows = case_rows[baseline_case]
        for scenario in AI_SCENARIOS:
            rows.append(energy_row(name, scenario, full_rows, base_rows,
                                   window_policy))

        rows.append(energy_row(name, "overall_ai", full_rows, base_rows,
                               window_policy))
    return rows


def droop_stat(rows, scenario, pct, window_policy=DEFAULT_WINDOW_POLICY):
    subset = selected_rows(rows, scenario, window_policy)
    return percentile((to_float(row, "droop_mv") for row in subset), pct)


def overshoot_stat(rows, scenario, pct, window_policy=DEFAULT_WINDOW_POLICY):
    subset = selected_rows(rows, scenario, window_policy)
    return percentile((to_float(row, "overshoot_mv") for row in subset), pct)


def voltage_limiter_mv(droop_mv, overshoot_mv, overshoot_allowance_mv,
                       overshoot_penalty):
    if not math.isfinite(droop_mv) or not math.isfinite(overshoot_mv):
        return math.nan
    overshoot_excess = max(0.0, overshoot_mv - overshoot_allowance_mv)
    return droop_mv + overshoot_penalty * overshoot_excess


def scenario_weight(rows, scenario, window_policy=DEFAULT_WINDOW_POLICY):
    subset = selected_rows(rows, scenario, window_policy)
    return avg(to_float(row, "avg_load_ma") for row in subset)


def fmax_score(veff_v, vth_v, alpha):
    if not math.isfinite(veff_v) or veff_v <= vth_v or veff_v <= 0.0:
        return math.nan
    return ((veff_v - vth_v) ** alpha) / veff_v


def tops_from_score(score, ref_score, nominal_tops):
    if not math.isfinite(score) or not math.isfinite(ref_score) or ref_score <= 0.0:
        return math.nan
    return nominal_tops * score / ref_score


def tops_per_w(tops, pin_mw):
    if not math.isfinite(tops) or not math.isfinite(pin_mw) or pin_mw <= 0.0:
        return math.nan
    return tops / (pin_mw * 1.0e-3)


def performance_row(name, scenario, full_rows, base_rows, vnom_v, vth_v,
                    alpha, droop_percentile, overshoot_allowance_mv,
                    overshoot_penalty, nominal_tops,
                    window_policy=DEFAULT_WINDOW_POLICY):
    full_droop_mv = droop_stat(full_rows, scenario, droop_percentile,
                               window_policy)
    base_droop_mv = droop_stat(base_rows, scenario, droop_percentile,
                               window_policy)
    full_overshoot_mv = overshoot_stat(full_rows, scenario, droop_percentile,
                                       window_policy)
    base_overshoot_mv = overshoot_stat(base_rows, scenario, droop_percentile,
                                       window_policy)
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
    ref_score = fmax_score(vnom_v, vth_v, alpha)
    full_tops = tops_from_score(full_score, ref_score, nominal_tops)
    base_tops = tops_from_score(base_score, ref_score, nominal_tops)
    full_power = scenario_power(full_rows, scenario, window_policy)
    base_power = scenario_power(base_rows, scenario, window_policy)
    full_tops_per_w = tops_per_w(full_tops, full_power["pin_mw"])
    base_tops_per_w = tops_per_w(base_tops, base_power["pin_mw"])
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
        "nominal_tops": fmt(nominal_tops),
        "full_tops": fmt(full_tops),
        "baseline_tops": fmt(base_tops),
        "full_tops_per_w": fmt(full_tops_per_w),
        "baseline_tops_per_w": fmt(base_tops_per_w),
        "f_ratio": fmt(f_ratio),
        "performance_improvement_pct": fmt(improvement),
        "weight": fmt(scenario_weight(full_rows, scenario, window_policy)
                      if scenario != "overall_ai" else math.nan),
        "status": "estimated" if math.isfinite(improvement) else "invalid",
        "note": f"alpha-power estimate using {window_policy} windows",
    }


def overall_performance_row(name, scenario_rows_, vnom_v, vth_v, alpha,
                            droop_percentile, overshoot_allowance_mv,
                            overshoot_penalty, nominal_tops):
    weighted = []
    full_tops_weighted = []
    base_tops_weighted = []
    for row in scenario_rows_:
        f_ratio = float(row["f_ratio"]) if row["f_ratio"] else math.nan
        weight = float(row["weight"]) if row["weight"] else math.nan
        if math.isfinite(f_ratio) and f_ratio > 0.0 and math.isfinite(weight) and weight > 0.0:
            weighted.append((weight, f_ratio))
            full_tops = float(row["full_tops"]) if row["full_tops"] else math.nan
            base_tops = float(row["baseline_tops"]) if row["baseline_tops"] else math.nan
            if math.isfinite(full_tops):
                full_tops_weighted.append((weight, full_tops))
            if math.isfinite(base_tops):
                base_tops_weighted.append((weight, base_tops))

    if weighted:
        total_weight = sum(weight for weight, _ in weighted)
        harmonic_speedup = total_weight / sum(weight / f_ratio for weight, f_ratio in weighted)
        improvement = (harmonic_speedup - 1.0) * 100.0
    else:
        total_weight = math.nan
        harmonic_speedup = math.nan
        improvement = math.nan
    complete = len(weighted) == len(scenario_rows_)
    full_tops = (
        sum(weight * value for weight, value in full_tops_weighted) /
        sum(weight for weight, _ in full_tops_weighted)
        if full_tops_weighted else math.nan
    )
    base_tops = (
        sum(weight * value for weight, value in base_tops_weighted) /
        sum(weight for weight, _ in base_tops_weighted)
        if base_tops_weighted else math.nan
    )

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
        "nominal_tops": fmt(nominal_tops),
        "full_tops": fmt(full_tops),
        "baseline_tops": fmt(base_tops),
        "full_tops_per_w": "",
        "baseline_tops_per_w": "",
        "f_ratio": fmt(harmonic_speedup),
        "performance_improvement_pct": fmt(improvement),
        "weight": fmt(total_weight),
        "status": "estimated" if math.isfinite(improvement) and complete else "invalid",
        "note": "load-current weighted harmonic mean of scenario f-ratios"
                if complete else "invalid because at least one scenario f-ratio is unavailable",
    }


def build_performance_summary(case_rows, vnom_v, vth_v, alpha,
                              droop_percentile, overshoot_allowance_mv,
                              overshoot_penalty, nominal_tops,
                              window_policy=DEFAULT_WINDOW_POLICY):
    full_rows = case_rows["full"]
    rows = []
    for name, baseline_case in COMPARISONS:
        base_rows = case_rows[baseline_case]
        comparison_rows = []
        for scenario in AI_SCENARIOS:
            row = performance_row(name, scenario, full_rows, base_rows,
                                  vnom_v, vth_v, alpha, droop_percentile,
                                  overshoot_allowance_mv, overshoot_penalty,
                                  nominal_tops, window_policy)
            comparison_rows.append(row)
            rows.append(row)
        rows.append(overall_performance_row(name, comparison_rows, vnom_v,
                                            vth_v, alpha, droop_percentile,
                                            overshoot_allowance_mv,
                                            overshoot_penalty, nominal_tops))
    return rows


def complete_paper_comparison(results_dir, energy_rows, performance_rows,
                              droop_percentile=DEFAULT_DROOP_PERCENTILE):
    source = results_dir / "full" / "pdn_paper_comparison.csv"
    if not source.exists():
        return []

    rows = read_rows(source)
    full_by_key = {(row["metric"], row["scenario"]): row for row in rows}
    full_learning_source = results_dir / "full" / "pdn_learning_metrics.csv"
    full_learning_rows = (
        read_rows(full_learning_source) if full_learning_source.exists() else []
    )

    def set_model_metric(metric, scenario, value, note=None):
        for row in rows:
            if row.get("metric") == metric and row.get("scenario") == scenario:
                row["model_value"] = fmt(value)
                if note is not None:
                    row["note"] = note
                return
        unit = ""
        if "reduction" in metric:
            unit = "%"
        elif metric in ("best_iteration", "best_pkpk_iteration"):
            unit = "iter"
        elif "droop" in metric or "pkpk" in metric:
            unit = "mV"
        rows.append({
            "metric": metric,
            "scenario": scenario,
            "paper_value": "",
            "model_value": fmt(value),
            "unit": unit,
            "note": note or "",
        })

    for scenario in AI_SCENARIOS:
        best_row = best_pkpk_row(full_learning_rows, scenario)
        if best_row is None:
            continue
        baseline_droop = to_float(best_row, "baseline_droop_mv")
        baseline_pkpk = to_float(best_row, "baseline_pkpk_mv")
        droop_at_best_pkpk = to_float(best_row, "droop_mv")
        best_pkpk = to_float(best_row, "pkpk_mv")
        best_iteration = to_float(best_row, "iteration")
        set_model_metric(
            "best_max_droop", scenario, droop_at_best_pkpk,
            "Droop at model best peak-to-peak window")
        set_model_metric(
            "best_iteration_droop", scenario, droop_at_best_pkpk,
            "Droop sampled at minimum peak-to-peak iteration")
        set_model_metric(
            "best_pkpk", scenario, best_pkpk,
            "Model best peak-to-peak window value")
        set_model_metric(
            "droop_reduction", scenario,
            pct_reduction(baseline_droop, droop_at_best_pkpk),
            "Computed from baseline droop and droop at best peak-to-peak window")
        set_model_metric(
            "pkpk_reduction", scenario,
            pct_reduction(baseline_pkpk, best_pkpk),
            "Computed from model baseline and best peak-to-peak values")
        set_model_metric(
            "best_iteration", scenario, best_iteration,
            "Learning window index selected by minimum peak-to-peak")

    full_by_key = {(row["metric"], row["scenario"]): row for row in rows}
    paired_rows = []

    baseline_source = results_dir / "baseline_all_off" / "pdn_paper_comparison.csv"
    baseline_learning_source = (
        results_dir / "baseline_all_off" / "pdn_learning_metrics.csv"
    )
    if baseline_source.exists():
        baseline_rows = read_rows(baseline_source)
        baseline_learning_rows = (
            read_rows(baseline_learning_source)
            if baseline_learning_source.exists() else []
        )
        baseline_by_key = {
            (row["metric"], row["scenario"]): row for row in baseline_rows
        }

        def model_value(table, metric, scenario):
            row = table.get((metric, scenario))
            return to_float(row, "model_value") if row else math.nan

        def learning_values(table, scenario, key):
            return [
                to_float(row, key)
                for row in table
                if row.get("scenario") == scenario
            ]

        def learning_stat(table, scenario, key, pct):
            return percentile(learning_values(table, scenario, key), pct)

        def learning_final(table, scenario, key):
            values = [
                value for value in learning_values(table, scenario, key)
                if math.isfinite(value)
            ]
            return values[-1] if values else math.nan

        def paired_baseline_stat(scenario, metric):
            key = "droop_mv" if metric == "baseline_max_droop" else "pkpk_mv"
            values = learning_values(baseline_learning_rows, scenario, key)
            values = [value for value in values if math.isfinite(value)]
            if values:
                return max(values)
            return model_value(baseline_by_key, metric, scenario)

        def append_paired(metric, scenario, value, unit, note):
            paired_rows.append({
                "metric": metric,
                "scenario": scenario,
                "paper_value": "",
                "model_value": fmt(value),
                "unit": unit,
                "note": note,
            })

        for scenario in AI_SCENARIOS:
            baseline_droop = paired_baseline_stat(scenario,
                                                  "baseline_max_droop")
            best_droop = model_value(full_by_key, "best_max_droop",
                                     scenario)
            baseline_pkpk = paired_baseline_stat(scenario, "baseline_pkpk")
            best_pkpk = model_value(full_by_key, "best_pkpk", scenario)

            append_paired("paired_baseline_max_droop", scenario,
                          baseline_droop, "mV",
                          "baseline_all_off max droop across windows")
            append_paired("paired_full_best_droop", scenario,
                          best_droop, "mV",
                          "full-case droop at best peak-to-peak window")
            droop_reduction = pct_reduction(baseline_droop, best_droop)
            append_paired("paired_droop_reduction", scenario,
                          droop_reduction, "%",
                          "full droop at best peak-to-peak vs baseline_all_off max droop")

            append_paired("paired_baseline_pkpk", scenario,
                          baseline_pkpk, "mV",
                          "baseline_all_off max peak-to-peak across windows")
            append_paired("paired_full_best_pkpk", scenario,
                          best_pkpk, "mV",
                          "full-case best peak-to-peak window")
            pkpk_reduction = pct_reduction(baseline_pkpk, best_pkpk)
            append_paired("paired_pkpk_reduction", scenario,
                          pkpk_reduction, "%",
                          "full best peak-to-peak vs baseline_all_off max peak-to-peak")

            baseline_p95 = learning_stat(baseline_learning_rows, scenario,
                                         "droop_mv", droop_percentile)
            full_p95 = learning_stat(full_learning_rows, scenario,
                                     "droop_mv", droop_percentile)
            append_paired("paired_baseline_p95_droop", scenario,
                          baseline_p95, "mV",
                          f"baseline_all_off p{droop_percentile:g} droop")
            append_paired("paired_full_p95_droop", scenario,
                          full_p95, "mV",
                          f"full-case p{droop_percentile:g} droop")
            append_paired("paired_p95_droop_reduction", scenario,
                          pct_reduction(baseline_p95, full_p95), "%",
                          "stable p95 droop reduction vs baseline_all_off")

            baseline_final = learning_final(baseline_learning_rows, scenario,
                                            "droop_mv")
            full_final = learning_final(full_learning_rows, scenario,
                                        "droop_mv")
            append_paired("paired_baseline_final_droop", scenario,
                          baseline_final, "mV",
                          "baseline_all_off final-window droop")
            append_paired("paired_full_final_droop", scenario,
                          full_final, "mV",
                          "full-case final-window droop")
            append_paired("paired_final_droop_reduction", scenario,
                          pct_reduction(baseline_final, full_final), "%",
                          "final-window droop reduction vs baseline_all_off")

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
    return rows + paired_rows


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
    parser.add_argument("--nominal-tops", type=float,
                        default=DEFAULT_NOMINAL_TOPS,
                        help="Nominal compute throughput at the reference voltage.")
    parser.add_argument("--window-policy",
                        choices=("all", "sim_best", "paper_iter", "converged_done",
                                 "final_stable"),
                        default=DEFAULT_WINDOW_POLICY,
                        help="Which learning windows are used for summaries.")
    args = parser.parse_args()

    results_dir = Path(args.results_dir)
    case_rows = {case: load_case(results_dir, case) for case in CASES}

    case_summary = []
    for case, rows in case_rows.items():
        case_summary.extend(summarize_case(case, rows, args.window_policy))
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
            "best_pkpk_iteration",
            "baseline_droop_mv",
            "droop_at_best_pkpk_mv",
            "droop_reduction_at_best_pkpk_pct",
            "avg_load_ma",
        ),
        case_summary,
    )

    energy_summary = build_energy_summary(case_rows, args.window_policy)
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
        args.overshoot_penalty, args.nominal_tops, args.window_policy)
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
            "nominal_tops",
            "full_tops",
            "baseline_tops",
            "full_tops_per_w",
            "baseline_tops_per_w",
            "f_ratio",
            "performance_improvement_pct",
            "weight",
            "status",
            "note",
        ),
        performance_summary,
    )

    completed = complete_paper_comparison(
        results_dir, energy_summary, performance_summary,
        args.droop_percentile)
    if completed:
        write_rows(
            results_dir / "paper_comparison_completed.csv",
            ("metric", "scenario", "paper_value", "model_value", "unit", "note"),
            completed,
        )

    print(f"Wrote summaries to {results_dir}")


if __name__ == "__main__":
    main()
