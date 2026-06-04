#include <cstdio>
#include <cstdlib>
#include <cstddef>
#include <iomanip>
#include <iostream>
#include <string>

#include <systemc>
#include <systemc-ams>

#include "../source/distributed_pdn_system.h"
#include "../source/const_src.h"
#include "../source/pdn_metrics.h"

namespace {

bool starts_with(const std::string& value, const char* prefix)
{
    const std::string p(prefix);
    return value.compare(0, p.size(), p) == 0;
}

double parse_double_option(const std::string& arg, const char* prefix,
                           double fallback)
{
    if (!starts_with(arg, prefix))
        return fallback;
    return std::atof(arg.substr(std::string(prefix).size()).c_str());
}

int scenario_name_to_index(const std::string& name)
{
    if (name == "all")
        return -1;
    if (name == "dense_10pct_75pct")
        return 0;
    if (name == "dense_mid_30pct_62p5pct")
        return 1;
    if (name == "medium_50pct_50pct")
        return 2;
    if (name == "sparse_mid_70pct_37p5pct")
        return 3;
    if (name == "sparse_90pct_25pct")
        return 4;
    if (name == "load_step_116_506_330ma" || name == "load_step")
        return 5;
    return -2;
}

double calibrated_dynamic_current(double target_dense_avg_current,
                                  double leakage_current,
                                  std::size_t clusters,
                                  double dense_activity_gain,
                                  double burst_low_ratio)
{
    const double dense_density = (1.0 - 0.10) * 0.75;
    const double burst_average = (1.0 + burst_low_ratio) * 0.5;
    const double spatial_sum = 8.35;
    const double dynamic_gain =
        dense_density * dense_activity_gain * burst_average * spatial_sum;
    const double leakage_total = static_cast<double>(clusters) * leakage_current;
    const double dynamic =
        (target_dense_avg_current - leakage_total) / dynamic_gain;
    return dynamic > 0.0 ? dynamic : 0.0;
}

double calibrated_background_current(double target_dense_avg_current,
                                     double target_sparse_avg_current,
                                     std::size_t clusters,
                                     double dense_activity_gain,
                                     double sparse_activity_gain,
                                     double burst_low_ratio)
{
    const double dense_density = (1.0 - 0.10) * 0.75;
    const double sparse_density = (1.0 - 0.90) * 0.25;
    const double burst_average = (1.0 + burst_low_ratio) * 0.5;
    const double spatial_sum = 8.35;
    const double dense_effective_density =
        dense_density * dense_activity_gain;
    const double sparse_effective_density =
        sparse_density * sparse_activity_gain;
    const double density_delta =
        dense_effective_density - sparse_effective_density;
    if (density_delta <= 0.0 || clusters == 0)
        return 0.0;

    const double dynamic =
        (target_dense_avg_current - target_sparse_avg_current) /
        (burst_average * spatial_sum * density_delta);
    const double sparse_dynamic =
        dynamic * burst_average * spatial_sum * sparse_effective_density;
    const double background_total =
        target_sparse_avg_current - sparse_dynamic;
    return background_total > 0.0 ?
           background_total / static_cast<double>(clusters) : 0.0;
}

} // namespace

int sc_main(int argc, char* argv[])
{
    sc_core::sc_set_time_resolution(100.0, sc_core::SC_FS);

    bool enable_eec = true;
    bool enable_learning = true;
    bool enable_balance = true;
    bool use_paper_fixed_profile = false;
    bool enable_trace = true;
    int forced_scenario_index = -1;
    std::string scenario_name = "all";
    const unsigned learn_window_cycles = 256;
    const unsigned scenario_windows = 32;
    const unsigned warmup_windows = 4;
    const unsigned warmup_cycles = warmup_windows * learn_window_cycles;
    const unsigned step_windows = 3;
    const double sys_clk_period_ns = 10.0;
    const double nominal_vref = 0.8;
    const double vin_value = 0.9;
    double paper_dense_avg_current = 440.0e-3;
    double paper_sparse_avg_proxy = 0.0;
    double dense_activity_gain = 1.0;
    double dense_mid_activity_gain = 1.35;
    double medium_activity_gain = 1.35;
    double sparse_mid_activity_gain = 2.40;
    double sparse_activity_gain = 2.40;
    double burst_low_ratio = 0.20;
    const double eec_latency_bound_ps = 200.0;
    double grid_resistance = 50.0e-3;
    double local_cout = 3.3e-9;
    double grid_decap = 0.5e-9;
    bool dynamic_current_overridden = false;
    bool leakage_current_overridden = false;
    bool sparse_target_overridden = false;
    double workload_dynamic_current = 0.0;
    double workload_leakage_current = 0.0;

    for (int i = 1; i < argc; ++i) {
        const std::string arg(argv[i]);
        if (arg == "--disable-eec") {
            enable_eec = false;
        } else if (arg == "--disable-learning") {
            enable_learning = false;
        } else if (arg == "--disable-balance") {
            enable_balance = false;
        } else if (arg == "--paper-no-learning-profile") {
            use_paper_fixed_profile = true;
        } else if (arg == "--no-trace") {
            enable_trace = false;
        } else if (starts_with(arg, "--scenario=")) {
            scenario_name = arg.substr(std::string("--scenario=").size());
            forced_scenario_index = scenario_name_to_index(scenario_name);
            if (forced_scenario_index < -1) {
                std::cerr << "Unknown scenario: " << scenario_name << "\n";
                return 1;
            }
        } else if (starts_with(arg, "--dynamic-current-ma=")) {
            workload_dynamic_current =
                parse_double_option(arg, "--dynamic-current-ma=", 18.0) * 1.0e-3;
            dynamic_current_overridden = true;
        } else if (starts_with(arg, "--leakage-current-ma=")) {
            workload_leakage_current =
                parse_double_option(arg, "--leakage-current-ma=",
                                     workload_leakage_current * 1.0e3) * 1.0e-3;
            leakage_current_overridden = true;
        } else if (starts_with(arg, "--dense-avg-current-ma=")) {
            paper_dense_avg_current =
                parse_double_option(arg, "--dense-avg-current-ma=", 440.0) * 1.0e-3;
        } else if (starts_with(arg, "--target-sparse-avg-ma=")) {
            paper_sparse_avg_proxy =
                parse_double_option(arg, "--target-sparse-avg-ma=", 275.0) * 1.0e-3;
            sparse_target_overridden = true;
        } else if (starts_with(arg, "--activity-gain-dense=")) {
            dense_activity_gain =
                parse_double_option(arg, "--activity-gain-dense=", 1.0);
        } else if (starts_with(arg, "--activity-gain-dense-mid=")) {
            dense_mid_activity_gain =
                parse_double_option(arg, "--activity-gain-dense-mid=", 1.35);
        } else if (starts_with(arg, "--activity-gain-medium=")) {
            medium_activity_gain =
                parse_double_option(arg, "--activity-gain-medium=", 1.35);
        } else if (starts_with(arg, "--activity-gain-sparse-mid=")) {
            sparse_mid_activity_gain =
                parse_double_option(arg, "--activity-gain-sparse-mid=", 2.40);
        } else if (starts_with(arg, "--activity-gain-sparse=")) {
            sparse_activity_gain =
                parse_double_option(arg, "--activity-gain-sparse=", 2.40);
        } else if (starts_with(arg, "--medium-activity-gain=")) {
            dense_mid_activity_gain =
                parse_double_option(arg, "--medium-activity-gain=", 1.35);
            medium_activity_gain = dense_mid_activity_gain;
        } else if (starts_with(arg, "--sparse-activity-gain=")) {
            sparse_mid_activity_gain =
                parse_double_option(arg, "--sparse-activity-gain=", 2.40);
            sparse_activity_gain = sparse_mid_activity_gain;
        } else if (starts_with(arg, "--burst-low-ratio=")) {
            burst_low_ratio =
                parse_double_option(arg, "--burst-low-ratio=", 0.20);
            if (burst_low_ratio < 0.0)
                burst_low_ratio = 0.0;
            else if (burst_low_ratio > 1.0)
                burst_low_ratio = 1.0;
        } else if (starts_with(arg, "--grid-resistance-mohm=")) {
            grid_resistance =
                parse_double_option(arg, "--grid-resistance-mohm=", 50.0) * 1.0e-3;
        } else if (starts_with(arg, "--local-cout-nf=")) {
            local_cout =
                parse_double_option(arg, "--local-cout-nf=", 3.3) * 1.0e-9;
        } else if (starts_with(arg, "--grid-decap-nf=")) {
            grid_decap =
                parse_double_option(arg, "--grid-decap-nf=", 0.5) * 1.0e-9;
        }
    }

    if (!sparse_target_overridden)
        paper_sparse_avg_proxy = paper_dense_avg_current * 80.0 / 128.0;

    if (!leakage_current_overridden) {
        workload_leakage_current =
            calibrated_background_current(paper_dense_avg_current,
                                          paper_sparse_avg_proxy, 9,
                                          dense_activity_gain,
                                          sparse_activity_gain,
                                          burst_low_ratio);
    }

    if (!dynamic_current_overridden) {
        workload_dynamic_current =
            calibrated_dynamic_current(paper_dense_avg_current,
                                       workload_leakage_current, 9,
                                       dense_activity_gain,
                                       burst_low_ratio);
    }

    sc_core::sc_clock clk_sys("clk_sys",
                              sc_core::sc_time(sys_clk_period_ns,
                                               sc_core::SC_NS));
    sc_core::sc_signal<bool> rst_n("rst_n");

    sca_eln::sca_node n_vin;
    sca_eln::sca_node_ref gnd;

    sca_tdf::sca_signal<double> vin_tdf("vin_tdf");
    const_src vin_src_tdf("vin_src_tdf");
    vin_src_tdf.value = vin_value;
    vin_src_tdf.outp(vin_tdf);

    sca_eln::sca_tdf_vsource vin_src("vin_src", 1.0);
    vin_src.p(n_vin);
    vin_src.n(gnd);
    vin_src.inp(vin_tdf);

    sc_core::sc_vector<sc_core::sc_signal<double>> load_current("load_current", 9);
    sc_core::sc_vector<sc_core::sc_signal<double>> activity("activity", 9);
    sc_core::sc_signal<double> weight_sparsity("weight_sparsity");
    sc_core::sc_signal<double> input_toggle("input_toggle");

    paper_workload workload("workload", 9, scenario_windows,
                            learn_window_cycles,
                            workload_leakage_current,
                            workload_dynamic_current,
                            dense_activity_gain,
                            dense_mid_activity_gain,
                            medium_activity_gain,
                            sparse_mid_activity_gain,
                            sparse_activity_gain,
                            burst_low_ratio,
                            warmup_cycles);
    workload.forced_scenario_index = forced_scenario_index;
    workload.clk(clk_sys);
    workload.rst_n(rst_n);
    workload.weight_sparsity(weight_sparsity);
    workload.input_toggle(input_toggle);
    for (std::size_t i = 0; i < 9; ++i) {
        workload.load_current[i](load_current[i]);
        workload.activity[i](activity[i]);
    }

    distributed_pdn_system dut("dut", 9, nominal_vref,
                               grid_resistance, local_cout, grid_decap,
                               warmup_cycles);
    dut.clk_sys(clk_sys);
    // dut.clk_eec(clk_eec);
    dut.rst_n(rst_n);
    dut.vdd(n_vin);
    dut.vss(gnd);
    dut.weight_sparsity(weight_sparsity);
    dut.input_toggle(input_toggle);
    for (std::size_t i = 0; i < 9; ++i) {
        dut.load_current[i](load_current[i]);
        dut.activity[i](activity[i]);
    }
    dut.apply_runtime_options(enable_eec, enable_learning, enable_balance,
                              use_paper_fixed_profile);

    pdn_paper_monitor metrics("metrics", 9, 16, nominal_vref, vin_value,
                              learn_window_cycles, warmup_cycles,
                              eec_latency_bound_ps);
    metrics.eec_enabled = enable_eec;
    metrics.learning_enabled = enable_learning;
    metrics.balance_enabled = enable_balance;
    metrics.clk_sys(clk_sys);
    // metrics.clk_eec(clk_eec);
    metrics.rst_n(rst_n);
    metrics.avg_vout(dut.avg_vout_de);
    metrics.weight_sparsity(weight_sparsity);
    metrics.input_toggle(input_toggle);
    metrics.vrefh(dut.learned_vrefh);
    metrics.vrefl(dut.learned_vrefl);
    metrics.vdrp(dut.learned_vdrp);
    metrics.vos(dut.learned_vos);
    metrics.global_code(dut.global_code);
    metrics.ctie_hi(dut.learned_ctie_hi);
    metrics.ctie_lo(dut.learned_ctie_lo);
    metrics.learning_phase(dut.learned_phase);
    for (std::size_t i = 0; i < 9; ++i) {
        metrics.vout[i](dut.vout_de[i]);
        metrics.load_current[i](load_current[i]);
        metrics.activity[i](activity[i]);
        metrics.local_code[i](dut.lldo[i].local_code);
        metrics.code_sum[i](dut.lldo[i].code_sum);
        metrics.gate_word[i](dut.lldo[i].gate_word);
        metrics.balance_adjust[i](dut.balance_adjust[i]);
        metrics.event_under[i](dut.lldo[i].event_under_d);
        metrics.event_over[i](dut.lldo[i].event_over_d);
        metrics.event_active[i](dut.lldo[i].mode_event_active);
        for (std::size_t j = 0; j < 16; ++j) {
            metrics.finger_current[i * 16 + j](
                dut.lldo[i].dldo_core.leg_[j].pm_.ids());
        }
    }

    sca_util::sca_trace_file* tf = nullptr;
    if (enable_trace) {
        tf = sca_util::sca_create_vcd_trace_file("waveform_pdn_system");
        sca_util::sca_trace(tf, dut.avg_vout_de, "avg_vout");
        sca_util::sca_trace(tf, dut.global_code, "global_code");
        sca_util::sca_trace(tf, dut.learned_vrefh, "vrefh");
        sca_util::sca_trace(tf, dut.learned_vrefl, "vrefl");
        sca_util::sca_trace(tf, dut.learned_vdrp, "vdrp");
        sca_util::sca_trace(tf, dut.learned_vos, "vos");
        sca_util::sca_trace(tf, dut.learned_ctie_hi, "ctie_hi");
        sca_util::sca_trace(tf, dut.learned_ctie_lo, "ctie_lo");
        sca_util::sca_trace(tf, dut.learned_phase, "learning_phase");
        sca_util::sca_trace(tf, weight_sparsity, "weight_sparsity");
        sca_util::sca_trace(tf, input_toggle, "input_toggle");

        for (std::size_t i = 0; i < 9; ++i) {
            char name[64];
            std::snprintf(name, sizeof(name), "vout_%zu", i);
            sca_util::sca_trace(tf, dut.vout_de[i], name);
            std::snprintf(name, sizeof(name), "load_%zu", i);
            sca_util::sca_trace(tf, load_current[i], name);
            std::snprintf(name, sizeof(name), "local_code_%zu", i);
            sca_util::sca_trace(tf, dut.lldo[i].local_code, name);
            std::snprintf(name, sizeof(name), "gate_word_%zu", i);
            sca_util::sca_trace(tf, dut.lldo[i].gate_word, name);
            std::snprintf(name, sizeof(name), "code_sum_%zu", i);
            sca_util::sca_trace(tf, dut.lldo[i].code_sum, name);
            std::snprintf(name, sizeof(name), "balance_%zu", i);
            sca_util::sca_trace(tf, dut.balance_adjust[i], name);
            std::snprintf(name, sizeof(name), "event_under_%zu", i);
            sca_util::sca_trace(tf, dut.lldo[i].event_under_d, name);
            std::snprintf(name, sizeof(name), "event_over_%zu", i);
            sca_util::sca_trace(tf, dut.lldo[i].event_over_d, name);
            std::snprintf(name, sizeof(name), "event_active_%zu", i);
            sca_util::sca_trace(tf, dut.lldo[i].mode_event_active, name);
        }
    }

    std::cout << "Distributed PDN SystemC-AMS paper-comparison simulation started...\n"
              << "Options: EEC=" << (enable_eec ? "on" : "off")
              << ", learning=" << (enable_learning ? "on" : "off")
              << ", balance=" << (enable_balance ? "on" : "off")
              << ", paper_fixed_profile="
              << (use_paper_fixed_profile ? "on" : "off")
              << ", scenario=" << scenario_name
              << ", trace=" << (enable_trace ? "on" : "off")
              << ", dynamic_current="
              << workload_dynamic_current * 1.0e3 << " mA/cluster"
              << ", background_current="
              << workload_leakage_current * 1.0e3 << " mA/cluster"
              << ", dense_avg_target="
              << paper_dense_avg_current * 1.0e3 << " mA"
              << ", sparse_avg_target="
              << paper_sparse_avg_proxy * 1.0e3 << " mA"
              << ", activity_gains=["
              << dense_activity_gain << ", "
              << dense_mid_activity_gain << ", "
              << medium_activity_gain << ", "
              << sparse_mid_activity_gain << ", "
              << sparse_activity_gain << "]"
              << ", burst_low_ratio=" << burst_low_ratio
              << ", grid_resistance="
              << grid_resistance * 1.0e3 << " mOhm"
              << ", local_cout="
              << local_cout * 1.0e9 << " nF"
              << ", grid_decap="
              << grid_decap * 1.0e9 << " nF"
              << ", warmup=" << warmup_windows << " windows\n";
    rst_n.write(false);
    sc_core::sc_start(10.0, sc_core::SC_NS);

    rst_n.write(true);
    unsigned active_cycles = warmup_cycles;
    if (forced_scenario_index >= 0) {
        active_cycles +=
            (forced_scenario_index == 5 ? step_windows : scenario_windows) *
            learn_window_cycles;
    } else {
        active_cycles +=
            scenario_windows * learn_window_cycles * 5 +
            step_windows * learn_window_cycles;
    }
    const double active_time_ns =
        static_cast<double>(active_cycles) * sys_clk_period_ns;
    sc_core::sc_start(active_time_ns, sc_core::SC_NS);

    sc_core::sc_stop();
    if (tf) {
        sca_util::sca_close_vcd_trace_file(tf);
        tf = nullptr;
    }

    std::cout << std::fixed << std::setprecision(6)
              << "Simulation completed. avg_vout=" << dut.avg_vout_de.read()
              << " V, global_code=" << dut.global_code.read().to_uint()
              << ", VREFL=" << dut.learned_vrefl.read()
              << ", VREFH=" << dut.learned_vrefh.read() << "\n"
              << "Metrics written to pdn_learning_metrics.csv and "
              << "pdn_paper_comparison.csv\n";

    return 0;
}
