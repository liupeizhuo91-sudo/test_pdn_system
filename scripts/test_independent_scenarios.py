#!/usr/bin/env python3
"""Regression checks for independent scenario simulation wiring.

These checks intentionally exercise the source-level contract because the
SystemC-AMS toolchain is not always available in the local Python environment.
They guard the experiment-level behavior that each paper sparsity point can be
run in its own fresh simulator process before Makefile aggregation.
"""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(relative_path):
    return (ROOT / relative_path).read_text(encoding="utf-8")


def test_workload_can_force_one_scenario():
    source = read("source/pdn_metrics.h")

    assert "int forced_scenario_index;" in source
    assert "if (forced_scenario_index >= 0)" in source
    assert "return forced_scenario_index;" in source
    assert "forced_scenario_index == 5" in source


def test_top_exposes_scenario_option_and_shortens_runtime():
    source = read("test/test_top.cpp")

    assert "--scenario=" in source
    assert "scenario_name_to_index" in source
    assert "workload.forced_scenario_index = forced_scenario_index;" in source
    assert "forced_scenario_index >= 0" in source
    assert "scenario_windows * learn_window_cycles * 5" in source


def test_makefile_runs_each_scenario_in_a_fresh_process():
    source = read("build/Makefile")

    for scenario in (
        "dense_10pct_75pct",
        "dense_mid_30pct_62p5pct",
        "medium_50pct_50pct",
        "sparse_mid_70pct_37p5pct",
        "sparse_90pct_25pct",
    ):
        assert scenario in source

    assert "--scenario=$$$$scenario" in source
    assert "$(RESULTS_DIR)/$(1)/$$$$scenario" in source
    assert "tail -n +2 pdn_learning_metrics.csv" in source


def test_activity_gain_can_be_tuned_per_scenario():
    workload = read("source/pdn_metrics.h")
    top = read("test/test_top.cpp")
    makefile = read("build/Makefile")

    for field in (
        "dense_activity_gain",
        "dense_mid_activity_gain",
        "medium_activity_gain",
        "sparse_mid_activity_gain",
        "sparse_activity_gain",
    ):
        assert field in workload

    for option in (
        "--activity-gain-dense=",
        "--activity-gain-dense-mid=",
        "--activity-gain-medium=",
        "--activity-gain-sparse-mid=",
        "--activity-gain-sparse=",
    ):
        assert option in top

    for variable in (
        "DENSE_ACTIVITY_GAIN",
        "DENSE_MID_ACTIVITY_GAIN",
        "MEDIUM_ACTIVITY_GAIN",
        "SPARSE_MID_ACTIVITY_GAIN",
        "SPARSE_ACTIVITY_GAIN",
    ):
        assert variable in makefile
