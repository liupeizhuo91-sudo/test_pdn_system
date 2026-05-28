#ifndef PDN_METRICS_H
#define PDN_METRICS_H

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <fstream>
#include <iomanip>
#include <limits>
#include <string>
#include <vector>

#include <systemc>

SC_MODULE(paper_workload) {
    std::size_t num_clusters;
    unsigned scenario_windows;
    unsigned cycles_per_window;
    unsigned warmup_cycles;
    double leakage_current;
    double dynamic_current;
    double medium_activity_gain;
    double sparse_activity_gain;
    double burst_low_ratio;
    unsigned long long cycle;

    sc_core::sc_in<bool> clk;
    sc_core::sc_in<bool> rst_n;
    sc_core::sc_vector<sc_core::sc_out<double>> load_current;
    sc_core::sc_vector<sc_core::sc_out<double>> activity;
    sc_core::sc_out<double> weight_sparsity;
    sc_core::sc_out<double> input_toggle;

    double spatial_profile(std::size_t i) const
    {
        static const double profile[9] = {
            0.85, 1.10, 0.90,
            0.65, 1.35, 0.75,
            0.80, 1.25, 0.70
        };
        return profile[i % 9];
    }

    int scenario_index() const
    {
        const unsigned long long scenario_cycles =
            static_cast<unsigned long long>(scenario_windows) *
            static_cast<unsigned long long>(cycles_per_window);
        if (scenario_cycles == 0ULL)
            return 0;

        const unsigned long long effective_cycle =
            cycle >= warmup_cycles ? cycle - warmup_cycles : 0ULL;
        const unsigned long long index = effective_cycle / scenario_cycles;
        return static_cast<int>(std::min<unsigned long long>(index, 3ULL));
    }

    void scenario_values(int index, double& sparsity, double& toggle) const
    {
        if (index == 0) {
            sparsity = 0.10;
            toggle = 0.75;
        } else if (index == 1) {
            sparsity = 0.50;
            toggle = 0.50;
        } else if (index == 2) {
            sparsity = 0.90;
            toggle = 0.25;
        } else {
            sparsity = 0.00;
            toggle = 1.00;
        }
    }

    void write_load_step()
    {
        const unsigned long long scenario_cycles =
            static_cast<unsigned long long>(scenario_windows) *
            static_cast<unsigned long long>(cycles_per_window);
        const unsigned long long step_start =
            static_cast<unsigned long long>(warmup_cycles) +
            3ULL * scenario_cycles;
        const unsigned long long local_cycle =
            cycle > step_start ? cycle - step_start : 0ULL;

        double total_current = 116.0e-3;
        if (local_cycle >= 64ULL && local_cycle < 320ULL)
            total_current = 506.0e-3;
        else if (local_cycle >= 320ULL)
            total_current = 330.0e-3;

        weight_sparsity.write(0.0);
        input_toggle.write(1.0);

        const double clusters =
            static_cast<double>(std::max<std::size_t>(num_clusters, 1));
        const double per_cluster = total_current / clusters;
        const double max_cluster = 506.0e-3 / clusters;

        for (std::size_t i = 0; i < num_clusters; ++i) {
            load_current[i].write(per_cluster);
            activity[i].write(per_cluster / max_cluster);
        }
    }

    void write_scenario()
    {
        double sparsity = 0.10;
        double toggle = 0.75;
        const int index = scenario_index();
        if (index == 3) {
            write_load_step();
            return;
        }

        scenario_values(index, sparsity, toggle);

        weight_sparsity.write(sparsity);
        input_toggle.write(toggle);

        const unsigned long long effective_cycle =
            cycle >= warmup_cycles ? cycle - warmup_cycles : 0ULL;
        const bool burst_high = ((effective_cycle / 32ULL) % 2ULL) == 0ULL;
        const double burst = burst_high ? 1.0 : burst_low_ratio;
        const double density = std::max(0.0, 1.0 - sparsity) * toggle;
        double scenario_gain = 1.0;
        if (index == 1)
            scenario_gain = medium_activity_gain;
        else if (index == 2)
            scenario_gain = sparse_activity_gain;

        for (std::size_t i = 0; i < num_clusters; ++i) {
            const double act = std::min(
                1.0, density * burst * scenario_gain * spatial_profile(i));
            activity[i].write(act);
            load_current[i].write(leakage_current + dynamic_current * act);
        }
    }

    void workload_process()
    {
        if (!rst_n.read()) {
            cycle = 0;
            write_scenario();
            return;
        }

        write_scenario();
        ++cycle;
    }

    SC_HAS_PROCESS(paper_workload);
    paper_workload(sc_core::sc_module_name name_,
                   std::size_t num_clusters_ = 9,
                   unsigned scenario_windows_ = 32,
                   unsigned cycles_per_window_ = 256,
                   double leakage_current_ = 1.0e-3,
                   double dynamic_current_ = 18.0e-3,
                   double medium_activity_gain_ = 1.35,
                   double sparse_activity_gain_ = 2.40,
                   double burst_low_ratio_ = 0.20,
                   unsigned warmup_cycles_ = 0)
        : sc_module(name_),
          num_clusters(num_clusters_),
          scenario_windows(scenario_windows_),
          cycles_per_window(cycles_per_window_),
          warmup_cycles(warmup_cycles_),
          leakage_current(leakage_current_),
          dynamic_current(dynamic_current_),
          medium_activity_gain(medium_activity_gain_),
          sparse_activity_gain(sparse_activity_gain_),
          burst_low_ratio(burst_low_ratio_),
          cycle(0),
          clk("clk"),
          rst_n("rst_n"),
          load_current("load_current", num_clusters_),
          activity("activity", num_clusters_),
          weight_sparsity("weight_sparsity"),
          input_toggle("input_toggle")
    {
        SC_METHOD(workload_process);
        sensitive << clk.pos() << rst_n.neg();
        dont_initialize();
    }
};

SC_MODULE(pdn_paper_monitor) {
    struct ScenarioReport {
        std::string name;
        double paper_baseline_droop_mv;
        double paper_best_droop_mv;
        double paper_reduction_pct;
        unsigned paper_best_iter;

        bool seen;
        unsigned iteration;
        double baseline_droop_mv;
        double baseline_pkpk_mv;
        double best_droop_mv;
        double best_pkpk_mv;
        double droop_at_best_pkpk_mv;
        double overshoot_at_best_pkpk_mv;
        unsigned best_droop_iter;
        unsigned best_pkpk_iter;
        unsigned best_iter;
        double final_droop_mv;
        double final_pkpk_mv;
        double max_droop_mv;
        double max_overshoot_mv;
        double final_reduction_pct;
        double final_avg_load_ma;
        double mean_load_ma;
        double avg_load_accum_ma;
        double peak_load_ma;
        unsigned measured_windows;
        double min_event_latency_ps;
        double min_control_latency_ps;
        double min_settle_ns;
        double best_efficiency_pct;
        double best_current_efficiency_pct;
        double final_iq_est_ua;
        double min_current_stddev_ma;
        double best_current_stddev_ma;
        double final_avg_ctot_code;

        ScenarioReport()
            : name("unknown"),
              paper_baseline_droop_mv(0.0),
              paper_best_droop_mv(0.0),
              paper_reduction_pct(0.0),
              paper_best_iter(0),
              seen(false),
              iteration(0),
              baseline_droop_mv(0.0),
              baseline_pkpk_mv(0.0),
              best_droop_mv(std::numeric_limits<double>::max()),
              best_pkpk_mv(std::numeric_limits<double>::max()),
              droop_at_best_pkpk_mv(std::numeric_limits<double>::max()),
              overshoot_at_best_pkpk_mv(std::numeric_limits<double>::max()),
              best_droop_iter(0),
              best_pkpk_iter(0),
              best_iter(0),
              final_droop_mv(0.0),
              final_pkpk_mv(0.0),
              max_droop_mv(0.0),
              max_overshoot_mv(0.0),
              final_reduction_pct(0.0),
              final_avg_load_ma(0.0),
              mean_load_ma(0.0),
              avg_load_accum_ma(0.0),
              peak_load_ma(0.0),
              measured_windows(0),
              min_event_latency_ps(std::numeric_limits<double>::max()),
              min_control_latency_ps(std::numeric_limits<double>::max()),
              min_settle_ns(std::numeric_limits<double>::max()),
              best_efficiency_pct(0.0),
              best_current_efficiency_pct(0.0),
              final_iq_est_ua(0.0),
              min_current_stddev_ma(std::numeric_limits<double>::max()),
              best_current_stddev_ma(std::numeric_limits<double>::max()),
              final_avg_ctot_code(0.0)
        {}
    };

    std::size_t num_ldos;
    std::size_t fingers_per_ldo;
    double nominal_vref;
    double vin;
    unsigned learn_window_cycles;
    unsigned warmup_cycles;
    double eec_latency_bound_ps;
    double controller_iq_current;
    bool eec_enabled;
    bool learning_enabled;
    bool balance_enabled;

    sc_core::sc_in<bool> clk_sys;
    sc_core::sc_in<bool> rst_n;
    sc_core::sc_vector<sc_core::sc_in<double>> vout;
    sc_core::sc_vector<sc_core::sc_in<double>> load_current;
    sc_core::sc_vector<sc_core::sc_in<double>> activity;
    sc_core::sc_vector<sc_core::sc_in<double>> finger_current;
    sc_core::sc_in<double> avg_vout;
    sc_core::sc_in<double> weight_sparsity;
    sc_core::sc_in<double> input_toggle;
    sc_core::sc_in<double> vrefh;
    sc_core::sc_in<double> vrefl;
    sc_core::sc_in<double> vdrp;
    sc_core::sc_in<double> vos;
    sc_core::sc_in<sc_dt::sc_uint<16>> global_code;
    sc_core::sc_in<sc_dt::sc_uint<16>> ctie_hi;
    sc_core::sc_in<sc_dt::sc_uint<16>> ctie_lo;
    sc_core::sc_vector<sc_core::sc_in<sc_dt::sc_uint<16>>> local_code;
    sc_core::sc_vector<sc_core::sc_in<sc_dt::sc_uint<16>>> code_sum;
    sc_core::sc_vector<sc_core::sc_in<sc_dt::sc_uint<16>>> gate_word;
    sc_core::sc_vector<sc_core::sc_in<sc_dt::sc_int<17>>> balance_adjust;
    sc_core::sc_vector<sc_core::sc_in<bool>> event_under;
    sc_core::sc_vector<sc_core::sc_in<bool>> event_over;
    sc_core::sc_vector<sc_core::sc_in<bool>> event_active;

    std::ofstream learning_csv;
    std::ofstream comparison_csv;
    std::vector<ScenarioReport> reports;

    int active_scenario;
    unsigned warmup_count;
    bool warmup_done;
    unsigned sample_count;
    double window_sparsity;
    double window_toggle;
    double win_min_vout;
    double win_max_vout;
    double win_load_sum;
    double win_load_peak;
    double win_load_min;
    double win_load_max;
    double win_pin_energy;
    double win_pout_energy;
    double win_control_energy;
    double win_guardband_energy;
    double win_switching_energy;
    bool win_rail_violation;
    double win_current_eff_peak;
    double win_iq_sum;
    double win_current_stddev_min;
    double win_current_stddev_sum;
    double win_ctot_sum;
    double win_event_latency_ps;
    double win_control_latency_ps;
    double win_settle_ns;

    bool prev_eec_valid;
    double prev_total_load_eec;
    unsigned prev_code_total_eec;
    bool pending_event_latency;
    bool pending_control_latency;
    unsigned code_total_at_edge;
    sc_core::sc_time load_edge_time;

    bool prev_sys_valid;
    double prev_total_load_sys;
    bool prev_power_valid;
    unsigned prev_power_code_total;
    bool settle_pending;
    bool settle_left_band;
    sc_core::sc_time settle_edge_time;

    static bool has_measurement(double value)
    {
        return std::isfinite(value) &&
               value < (std::numeric_limits<double>::max() * 0.25);
    }

    static double finite_or_blank(double value)
    {
        return has_measurement(value) ? value : 0.0;
    }

    static double safe_pct(double numerator, double denominator)
    {
        return std::fabs(denominator) > 1.0e-18 ? numerator / denominator * 100.0 : 0.0;
    }

    static double clamp_double(double value, double lo, double hi)
    {
        return std::max(lo, std::min(value, hi));
    }

    static double normalize_ratio(double value)
    {
        const double ratio = value > 1.0 ? value / 100.0 : value;
        return clamp_double(ratio, 0.0, 1.0);
    }

    static unsigned popcount16(sc_dt::sc_uint<16> value)
    {
        unsigned count = 0;
        unsigned raw = value.to_uint();
        for (unsigned i = 0; i < 16; ++i) {
            count += raw & 1u;
            raw >>= 1;
        }
        return count;
    }

    int scenario_from_workload() const
    {
        const double sparsity = weight_sparsity.read();
        const double toggle = input_toggle.read();
        if (std::fabs(sparsity - 0.10) < 0.05 && std::fabs(toggle - 0.75) < 0.10)
            return 0;
        if (std::fabs(sparsity - 0.50) < 0.10 && std::fabs(toggle - 0.50) < 0.10)
            return 1;
        if (std::fabs(sparsity - 0.90) < 0.05 && std::fabs(toggle - 0.25) < 0.10)
            return 2;
        if (std::fabs(sparsity - 0.00) < 0.05 && std::fabs(toggle - 1.00) < 0.10)
            return 3;
        return 3;
    }

    void initialize_reports()
    {
        reports.assign(4, ScenarioReport());

        reports[0].name = "dense_10pct_75pct";
        reports[0].paper_baseline_droop_mv = 128.0;
        reports[0].paper_best_droop_mv = 65.3;
        reports[0].paper_reduction_pct = 49.0;
        reports[0].paper_best_iter = 29;

        reports[1].name = "medium_50pct_50pct";
        reports[1].paper_baseline_droop_mv = 104.0;
        reports[1].paper_best_droop_mv = 48.0;
        reports[1].paper_reduction_pct = 53.8;
        reports[1].paper_best_iter = 23;

        reports[2].name = "sparse_90pct_25pct";
        reports[2].paper_baseline_droop_mv = 80.0;
        reports[2].paper_best_droop_mv = 23.0;
        reports[2].paper_reduction_pct = 71.3;
        reports[2].paper_best_iter = 23;

        reports[3].name = "load_step_116_506_330ma";
        reports[3].paper_baseline_droop_mv = 35.0;
        reports[3].paper_best_droop_mv = 15.7;
    }

    void reset_window()
    {
        sample_count = 0;
        window_sparsity = 0.0;
        window_toggle = 0.0;
        win_min_vout = std::numeric_limits<double>::max();
        win_max_vout = -std::numeric_limits<double>::max();
        win_load_sum = 0.0;
        win_load_peak = 0.0;
        win_load_min = std::numeric_limits<double>::max();
        win_load_max = 0.0;
        win_pin_energy = 0.0;
        win_pout_energy = 0.0;
        win_control_energy = 0.0;
        win_guardband_energy = 0.0;
        win_switching_energy = 0.0;
        win_rail_violation = false;
        win_current_eff_peak = 0.0;
        win_iq_sum = 0.0;
        win_current_stddev_min = std::numeric_limits<double>::max();
        win_current_stddev_sum = 0.0;
        win_ctot_sum = 0.0;
        win_event_latency_ps = std::numeric_limits<double>::max();
        win_control_latency_ps = std::numeric_limits<double>::max();
        win_settle_ns = std::numeric_limits<double>::max();
    }

    double total_load() const
    {
        double sum = 0.0;
        for (std::size_t i = 0; i < num_ldos; ++i)
            sum += load_current[i].read();
        return sum;
    }

    unsigned code_total() const
    {
        unsigned total = 0;
        for (std::size_t i = 0; i < num_ldos; ++i)
            total += code_sum[i].read().to_uint();
        return total;
    }

    double source_current(std::size_t ldo_index) const
    {
        double sum = 0.0;
        const std::size_t base = ldo_index * fingers_per_ldo;
        for (std::size_t j = 0; j < fingers_per_ldo; ++j)
            sum += finger_current[base + j].read();
        return sum;
    }

    double total_source_current() const
    {
        double sum = 0.0;
        for (std::size_t i = 0; i < num_ldos; ++i)
            sum += source_current(i);
        return sum;
    }

    unsigned total_on_fingers() const
    {
        unsigned total = 0;
        for (std::size_t i = 0; i < num_ldos; ++i)
            total += 16u - popcount16(gate_word[i].read());
        return total;
    }

    unsigned active_event_count() const
    {
        unsigned total = 0;
        for (std::size_t i = 0; i < num_ldos; ++i) {
            if (event_under[i].read() || event_over[i].read() ||
                event_active[i].read())
                ++total;
        }
        return total;
    }

    double workload_density() const
    {
        const double sparsity = normalize_ratio(weight_sparsity.read());
        const double toggle = normalize_ratio(input_toggle.read());
        return clamp_double((1.0 - sparsity) * (0.5 + 0.5 * toggle),
                            0.0, 1.0);
    }

    double guardband_voltage() const
    {
        const double density = workload_density();
        double guardband = 0.015;
        if (!learning_enabled)
            guardband += 0.110 + 0.060 * density;
        if (!eec_enabled)
            guardband += 0.080 + 0.060 * density;
        if (!balance_enabled)
            guardband += 0.012 + 0.010 * density;
        return guardband;
    }

    double controller_iq(double code_activity) const
    {
        const double density = workload_density();
        const double event_ratio =
            static_cast<double>(active_event_count()) /
            static_cast<double>(std::max<std::size_t>(num_ldos, 1));

        double iq = controller_iq_current;
        iq += 35.0e-6; // global slow-loop bias
        iq += learning_enabled ? (45.0e-6 + 15.0e-6 * density) : 8.0e-6;
        iq += eec_enabled ?
              (60.0e-6 + 30.0e-6 * event_ratio + 20.0e-6 * density) :
              15.0e-6;
        iq += balance_enabled ? (25.0e-6 + 10.0e-6 * density) : 3.0e-6;
        iq += 400.0e-6 * clamp_double(code_activity, 0.0, 1.0);
        return iq;
    }

    double switching_power(unsigned code_delta) const
    {
        const double density = workload_density();
        const double normalized_delta =
            static_cast<double>(code_delta) /
            static_cast<double>(std::max<std::size_t>(num_ldos, 1) * 0xFFFFu);
        const double on_ratio =
            static_cast<double>(total_on_fingers()) /
            static_cast<double>(std::max<std::size_t>(num_ldos * fingers_per_ldo, 1));

        return vin * vin * (0.20e-3 * normalized_delta +
                            0.03e-3 * density * on_ratio);
    }

    double current_stddev() const
    {
        if (num_ldos == 0)
            return 0.0;

        double sum = 0.0;
        std::vector<double> currents(num_ldos, 0.0);
        for (std::size_t i = 0; i < num_ldos; ++i) {
            currents[i] = source_current(i);
            sum += currents[i];
        }

        const double mean = sum / static_cast<double>(num_ldos);
        double var = 0.0;
        for (std::size_t i = 0; i < num_ldos; ++i) {
            const double err = currents[i] - mean;
            var += err * err;
        }
        return std::sqrt(var / static_cast<double>(num_ldos));
    }

    void open_files()
    {
        learning_csv.open("pdn_learning_metrics.csv", std::ios::out);
        learning_csv
            << "scenario,sparsity_pct,toggle_pct,iteration,time_ns,mode,"
            << "vmin_mv,vmax_mv,droop_mv,overshoot_mv,pkpk_mv,"
            << "baseline_droop_mv,baseline_pkpk_mv,droop_reduction_pct,pkpk_reduction_pct,"
            << "avg_load_ma,peak_load_ma,delta_load_ma,event_latency_ps,control_latency_ps,settle_ns,"
            << "vrefh_mv,vrefl_mv,vdrp_mv,vos_mv,ctie_hi,ctie_lo,global_code,"
            << "pin_mw,pout_mw,efficiency_pct,current_eff_peak_pct,iq_est_ua,"
            << "control_power_mw,guardband_power_mw,switching_power_mw,"
            << "rail_violation,"
            << "current_stddev_ma,current_stddev_min_ma,avg_ctot_code\n";
    }

    std::string mode_string() const
    {
        std::string mode;
        mode += eec_enabled ? "eec" : "no_eec";
        mode += learning_enabled ? "+learning" : "+no_learning";
        mode += balance_enabled ? "+balance" : "+no_balance";
        return mode;
    }

    void write_learning_row(int scenario, unsigned iteration,
                            double droop_mv, double overshoot_mv,
                            double pkpk_mv, double avg_load_ma,
                            double pin_mw, double pout_mw,
                            double efficiency_pct,
                            double control_power_mw,
                            double guardband_power_mw,
                            double switching_power_mw)
    {
        ScenarioReport& report = reports[scenario];
        const double droop_reduction =
            safe_pct(report.baseline_droop_mv - droop_mv,
                     report.baseline_droop_mv);
        const double pkpk_reduction =
            safe_pct(report.baseline_pkpk_mv - pkpk_mv,
                     report.baseline_pkpk_mv);
        const double delta_load_ma =
            std::max(0.0, win_load_max - win_load_min) * 1.0e3;
        const double event_latency =
            finite_or_blank(win_event_latency_ps);
        const double control_latency =
            finite_or_blank(win_control_latency_ps);
        const double settle_ns = finite_or_blank(win_settle_ns);
        const double stddev_ma =
            sample_count == 0 ? 0.0 :
            (win_current_stddev_sum / static_cast<double>(sample_count)) * 1.0e3;
        const double stddev_min_ma = finite_or_blank(win_current_stddev_min) * 1.0e3;
        const double avg_ctot =
            sample_count == 0 ? 0.0 : win_ctot_sum / static_cast<double>(sample_count);

        learning_csv << std::fixed << std::setprecision(6)
                     << report.name << ","
                     << window_sparsity * 100.0 << ","
                     << window_toggle * 100.0 << ","
                     << iteration << ","
                     << sc_core::sc_time_stamp().to_seconds() * 1.0e9 << ","
                     << mode_string() << ","
                     << win_min_vout * 1.0e3 << ","
                     << win_max_vout * 1.0e3 << ","
                     << droop_mv << ","
                     << overshoot_mv << ","
                     << pkpk_mv << ","
                     << report.baseline_droop_mv << ","
                     << report.baseline_pkpk_mv << ","
                     << droop_reduction << ","
                     << pkpk_reduction << ","
                     << avg_load_ma << ","
                     << win_load_peak * 1.0e3 << ","
                     << delta_load_ma << ","
                     << event_latency << ","
                     << control_latency << ","
                     << settle_ns << ","
                     << vrefh.read() * 1.0e3 << ","
                     << vrefl.read() * 1.0e3 << ","
                     << vdrp.read() * 1.0e3 << ","
                     << vos.read() * 1.0e3 << ","
                     << ctie_hi.read().to_uint() << ","
                     << ctie_lo.read().to_uint() << ","
                     << global_code.read().to_uint() << ","
                     << pin_mw << ","
                     << pout_mw << ","
                     << efficiency_pct << ","
                     << win_current_eff_peak << ","
                     << (sample_count == 0 ? 0.0 :
                          win_iq_sum / static_cast<double>(sample_count) * 1.0e6) << ","
                     << control_power_mw << ","
                     << guardband_power_mw << ","
                     << switching_power_mw << ","
                     << (win_rail_violation ? 1 : 0) << ","
                     << stddev_ma << ","
                     << stddev_min_ma << ","
                     << avg_ctot
                     << "\n";
    }

    void finish_window()
    {
        if (sample_count == 0 || active_scenario < 0 ||
            active_scenario >= static_cast<int>(reports.size()))
            return;

        const double droop_mv =
            std::max(0.0, nominal_vref - win_min_vout) * 1.0e3;
        const double overshoot_mv =
            std::max(0.0, win_max_vout - nominal_vref) * 1.0e3;
        const double pkpk_mv = droop_mv + overshoot_mv;
        const double avg_load_ma =
            win_load_sum / static_cast<double>(sample_count) * 1.0e3;
        const double pin_mw =
            win_pin_energy / (static_cast<double>(sample_count) * 1.0e-9) * 1.0e3;
        const double pout_mw =
            win_pout_energy / (static_cast<double>(sample_count) * 1.0e-9) * 1.0e3;
        const double control_power_mw =
            win_control_energy / (static_cast<double>(sample_count) * 1.0e-9) * 1.0e3;
        const double guardband_power_mw =
            win_guardband_energy / (static_cast<double>(sample_count) * 1.0e-9) * 1.0e3;
        const double switching_power_mw =
            win_switching_energy / (static_cast<double>(sample_count) * 1.0e-9) * 1.0e3;
        const double efficiency_pct =
            win_pin_energy > 0.0 ? win_pout_energy / win_pin_energy * 100.0 : 0.0;
        const double avg_stddev_ma =
            sample_count == 0 ? 0.0 :
            (win_current_stddev_sum / static_cast<double>(sample_count)) * 1.0e3;

        ScenarioReport& report = reports[active_scenario];
        report.seen = true;
        ++report.iteration;
        ++report.measured_windows;
        if (report.iteration == 1) {
            report.baseline_droop_mv = droop_mv;
            report.baseline_pkpk_mv = pkpk_mv;
        }

        if (droop_mv < report.best_droop_mv) {
            report.best_droop_mv = droop_mv;
            report.best_droop_iter = report.iteration;
        }

        if (pkpk_mv < report.best_pkpk_mv) {
            report.best_pkpk_mv = pkpk_mv;
            report.droop_at_best_pkpk_mv = droop_mv;
            report.overshoot_at_best_pkpk_mv = overshoot_mv;
            report.best_pkpk_iter = report.iteration;
            report.best_iter = report.iteration;
        }

        report.final_droop_mv = droop_mv;
        report.final_pkpk_mv = pkpk_mv;
        report.max_droop_mv = std::max(report.max_droop_mv, droop_mv);
        report.max_overshoot_mv =
            std::max(report.max_overshoot_mv, overshoot_mv);
        report.final_reduction_pct =
            safe_pct(report.baseline_droop_mv - droop_mv,
                     report.baseline_droop_mv);
        report.final_avg_load_ma = avg_load_ma;
        report.avg_load_accum_ma += avg_load_ma;
        report.mean_load_ma =
            report.avg_load_accum_ma / static_cast<double>(report.measured_windows);
        report.peak_load_ma = std::max(report.peak_load_ma, win_load_peak * 1.0e3);
        report.best_efficiency_pct =
            std::max(report.best_efficiency_pct, efficiency_pct);
        report.best_current_efficiency_pct =
            std::max(report.best_current_efficiency_pct, win_current_eff_peak);
        report.final_iq_est_ua =
            sample_count == 0 ? 0.0 :
            win_iq_sum / static_cast<double>(sample_count) * 1.0e6;
        report.final_avg_ctot_code =
            sample_count == 0 ? 0.0 :
            win_ctot_sum / static_cast<double>(sample_count);
        if (win_event_latency_ps < report.min_event_latency_ps)
            report.min_event_latency_ps = win_event_latency_ps;
        if (win_control_latency_ps < report.min_control_latency_ps)
            report.min_control_latency_ps = win_control_latency_ps;
        if (win_settle_ns < report.min_settle_ns)
            report.min_settle_ns = win_settle_ns;
        if (win_current_stddev_min < report.min_current_stddev_ma)
            report.min_current_stddev_ma = win_current_stddev_min;
        if (avg_stddev_ma < report.best_current_stddev_ma)
            report.best_current_stddev_ma = avg_stddev_ma;

        write_learning_row(active_scenario, report.iteration, droop_mv,
                            overshoot_mv, pkpk_mv, avg_load_ma, pin_mw,
                           pout_mw, efficiency_pct, control_power_mw,
                           guardband_power_mw, switching_power_mw);
        reset_window();
    }

    void record_sys_sample()
    {
        if (sample_count == 0) {
            window_sparsity = weight_sparsity.read();
            window_toggle = input_toggle.read();
        }

        double local_min = std::numeric_limits<double>::max();
        double local_max = -std::numeric_limits<double>::max();
        for (std::size_t i = 0; i < num_ldos; ++i) {
            const double v = vout[i].read();
            local_min = std::min(local_min, v);
            local_max = std::max(local_max, v);
            if (v < -1.0e-6 || v > vin + 0.25)
                win_rail_violation = true;
        }
        win_min_vout = std::min(win_min_vout, local_min);
        win_max_vout = std::max(win_max_vout, local_max);

        const double load = total_load();
        win_load_sum += load;
        win_load_peak = std::max(win_load_peak, load);
        win_load_min = std::min(win_load_min, load);
        win_load_max = std::max(win_load_max, load);

        double pout = 0.0;
        for (std::size_t i = 0; i < num_ldos; ++i) {
            const double v_for_power = clamp_double(vout[i].read(), 0.0, vin);
            pout += v_for_power * load_current[i].read();
        }

        const double source = std::max(0.0, total_source_current());
        const double regulator_input_current = std::max(load, source);
        const unsigned current_code_total = code_total();
        const unsigned code_delta =
            prev_power_valid ?
            (current_code_total > prev_power_code_total ?
             current_code_total - prev_power_code_total :
             prev_power_code_total - current_code_total) :
            0u;
        const double code_activity =
            static_cast<double>(code_delta) /
            static_cast<double>(std::max<std::size_t>(num_ldos, 1) * 0xFFFFu);
        const double iq = controller_iq(code_activity);
        const double control_power = vin * iq;
        const double guardband_power = load * guardband_voltage();
        const double switch_power = switching_power(code_delta);
        const double pin = vin * regulator_input_current +
                           control_power + guardband_power + switch_power;
        const double i_input = regulator_input_current + iq +
                               (guardband_power + switch_power) / vin;
        const double dt = 1.0e-9;
        win_pin_energy += pin * dt;
        win_pout_energy += pout * dt;
        win_control_energy += control_power * dt;
        win_guardband_energy += guardband_power * dt;
        win_switching_energy += switch_power * dt;

        if (i_input > 0.0)
            win_current_eff_peak =
                std::max(win_current_eff_peak,
                         std::min(100.0, load / i_input * 100.0));
        win_iq_sum += iq;
        prev_power_valid = true;
        prev_power_code_total = current_code_total;

        const double stddev = current_stddev();
        win_current_stddev_sum += stddev;
        win_current_stddev_min = std::min(win_current_stddev_min, stddev);

        win_ctot_sum += static_cast<double>(code_total()) /
                        static_cast<double>(std::max<std::size_t>(num_ldos, 1));

        if (prev_sys_valid &&
            std::fabs(load - prev_total_load_sys) > 5.0e-3) {
            settle_pending = true;
            settle_left_band = false;
            settle_edge_time = sc_core::sc_time_stamp();
        }

        if (settle_pending) {
            const double err = std::fabs(avg_vout.read() - nominal_vref);
            if (err > 5.0e-3)
                settle_left_band = true;
            if (settle_left_band && err <= 5.0e-3) {
                const double settle_ns =
                    (sc_core::sc_time_stamp() - settle_edge_time).to_seconds() *
                    1.0e9;
                win_settle_ns = std::min(win_settle_ns, settle_ns);
                settle_pending = false;
            }
        }

        prev_sys_valid = true;
        prev_total_load_sys = load;
        ++sample_count;
    }

    void sys_process()
    {
        if (!rst_n.read()) {
            active_scenario = -1;
            warmup_count = 0;
            warmup_done = warmup_cycles == 0;
            reset_window();
            prev_sys_valid = false;
            prev_power_valid = false;
            settle_pending = false;
            settle_left_band = false;
            return;
        }

        if (warmup_count < warmup_cycles) {
            ++warmup_count;
            warmup_done = false;
            active_scenario = -1;
            reset_window();
            prev_sys_valid = false;
            prev_power_valid = false;
            settle_pending = false;
            settle_left_band = false;
            return;
        }
        warmup_done = true;

        const int scenario = scenario_from_workload();
        if (scenario != active_scenario) {
            active_scenario = scenario;
            reset_window();
            prev_sys_valid = false;
            prev_power_valid = false;
            settle_pending = false;
            settle_left_band = false;
        }

        record_sys_sample();
        if (sample_count >= learn_window_cycles)
            finish_window();
    }

    void eec_process()
    {
        if (!rst_n.read()) {
            prev_eec_valid = false;
            pending_event_latency = false;
            pending_control_latency = false;
            return;
        }

        if (!warmup_done) {
            prev_eec_valid = false;
            pending_event_latency = false;
            pending_control_latency = false;
            return;
        }

        const double load = total_load();
        const unsigned code = code_total();

        if (prev_eec_valid &&
            std::fabs(load - prev_total_load_eec) > 5.0e-3) {
            load_edge_time = sc_core::sc_time_stamp();
            pending_event_latency = true;
            pending_control_latency = true;
            code_total_at_edge = code;
        }

        bool any_event = false;
        for (std::size_t i = 0; i < num_ldos; ++i) {
            any_event = any_event || event_under[i].read() ||
                        event_over[i].read() || event_active[i].read();
        }

        if (pending_event_latency && any_event) {
            const double latency_ps =
                (sc_core::sc_time_stamp() - load_edge_time).to_seconds() *
                1.0e12;
            win_event_latency_ps = std::min(win_event_latency_ps, latency_ps);
            pending_event_latency = false;
        }

        if (pending_control_latency && code != code_total_at_edge) {
            const double latency_ps =
                (sc_core::sc_time_stamp() - load_edge_time).to_seconds() *
                1.0e12;
            win_control_latency_ps = std::min(win_control_latency_ps, latency_ps);
            pending_control_latency = false;
        }

        prev_eec_valid = true;
        prev_total_load_eec = load;
        prev_code_total_eec = code;
    }

    void write_comparison_metric(const std::string& metric,
                                 const std::string& scenario,
                                 double paper_value,
                                 double model_value,
                                 const std::string& unit,
                                 const std::string& note)
    {
        write_comparison_metric_optional(metric, scenario, true, paper_value,
                                         true, model_value, unit, note);
    }

    void write_comparison_metric_optional(const std::string& metric,
                                          const std::string& scenario,
                                          bool has_paper,
                                          double paper_value,
                                          bool has_model,
                                          double model_value,
                                          const std::string& unit,
                                          const std::string& note)
    {
        comparison_csv << std::fixed << std::setprecision(6)
                       << metric << "," << scenario << ",";
        if (has_paper)
            comparison_csv << paper_value;
        comparison_csv << ",";
        if (has_model)
            comparison_csv << model_value;
        comparison_csv << "," << unit << "," << note << "\n";
    }

    void end_of_simulation() override
    {
        finish_window();
        if (learning_csv.is_open())
            learning_csv.close();

        comparison_csv.open("pdn_paper_comparison.csv", std::ios::out);
        comparison_csv << "metric,scenario,paper_value,model_value,unit,note\n";

        for (std::size_t i = 0; i < 3 && i < reports.size(); ++i) {
            const ScenarioReport& r = reports[i];
            const bool measured = r.seen && r.measured_windows > 0;
            const bool has_best_droop =
                measured && has_measurement(r.best_droop_mv);
            const bool has_best_pkpk =
                measured && has_measurement(r.best_pkpk_mv) &&
                has_measurement(r.droop_at_best_pkpk_mv);
            write_comparison_metric_optional("baseline_max_droop", r.name,
                                             true, r.paper_baseline_droop_mv,
                                             measured, r.baseline_droop_mv, "mV",
                                             "Fig.9 first iteration baseline");
            write_comparison_metric_optional("best_max_droop", r.name,
                                             true, r.paper_best_droop_mv,
                                             has_best_pkpk,
                                             r.droop_at_best_pkpk_mv, "mV",
                                             "Droop at model best peak-to-peak window");
            write_comparison_metric_optional("best_iteration_droop", r.name,
                                             false, 0.0,
                                             has_best_pkpk,
                                             r.droop_at_best_pkpk_mv, "mV",
                                             "Droop sampled at minimum peak-to-peak iteration");
            write_comparison_metric_optional("min_droop", r.name,
                                             false, 0.0,
                                             has_best_droop, r.best_droop_mv, "mV",
                                             "Minimum droop across learning windows");
            write_comparison_metric_optional("baseline_pkpk", r.name,
                                             false, 0.0,
                                             measured, r.baseline_pkpk_mv, "mV",
                                             "Model peak-to-peak window value");
            write_comparison_metric_optional("best_pkpk", r.name,
                                             false, 0.0,
                                             has_best_pkpk, r.best_pkpk_mv, "mV",
                                             "Model best peak-to-peak window value");
            write_comparison_metric_optional(
                "pkpk_reduction", r.name,
                false, 0.0,
                has_best_pkpk && std::fabs(r.baseline_pkpk_mv) > 1.0e-18,
                safe_pct(r.baseline_pkpk_mv - r.best_pkpk_mv,
                         r.baseline_pkpk_mv),
                "%",
                "Computed from model baseline and best peak-to-peak values");
            write_comparison_metric_optional(
                "droop_reduction", r.name,
                true, r.paper_reduction_pct,
                has_best_pkpk && std::fabs(r.baseline_droop_mv) > 1.0e-18,
                safe_pct(r.baseline_droop_mv - r.droop_at_best_pkpk_mv,
                         r.baseline_droop_mv),
                "%",
                "Computed from baseline droop and droop at best peak-to-peak window");
            write_comparison_metric_optional("best_iteration", r.name,
                                             true, static_cast<double>(r.paper_best_iter),
                                             has_best_pkpk,
                                             static_cast<double>(r.best_iter), "iter",
                                             "Learning window index selected by minimum peak-to-peak");
            write_comparison_metric_optional("avg_load_current", r.name,
                                             i == 0, 440.0,
                                             measured, r.mean_load_ma, "mA",
                                             i == 0 ? "Fig.8 dense average current target" :
                                                      "No explicit paper value for this scenario");
            write_comparison_metric_optional("peak_load_current", r.name,
                                             false, 0.0,
                                             measured, r.peak_load_ma, "mA",
                                             "Model workload peak; paper reports dense average current");
            write_comparison_metric("event_latency", r.name,
                                    400.0,
                                    eec_enabled ? eec_latency_bound_ps : 0.0,
                                    "ps",
                                    "Modeled EEC clock-period bound; paper critical path is <400ps");
            write_comparison_metric_optional(
                "best_current_stddev", r.name,
                true, 5.0,
                measured && has_measurement(r.best_current_stddev_ma),
                r.best_current_stddev_ma,
                "mA",
                "Best window average source-current sigma; paper target is <5mA");
            write_comparison_metric_optional(
                "best_efficiency", r.name,
                true, 99.9,
                measured, r.best_current_efficiency_pct, "%",
                "Peak current efficiency estimated as Iload/(Iload+Iq)");
        }

        const bool dense_measured =
            !reports.empty() && reports[0].seen && reports[0].measured_windows > 0;
        write_comparison_metric_optional(
            "eec_dense_pkpk", "dense_10pct_75pct",
            true, 128.0,
            dense_measured, reports[0].baseline_pkpk_mv, "mV",
            "First measured dense window after warm-up; use --disable-learning for pure w/o-learning mode");
        const bool step_measured =
            reports.size() > 3 && reports[3].seen && reports[3].measured_windows > 0;
        write_comparison_metric_optional("resistor_step_undershoot", "standalone_step",
                                         true, 35.0,
                                         step_measured, reports[3].max_droop_mv,
                                         "mV",
                                         "Model step phase uses total load 116mA->506mA");
        write_comparison_metric_optional("resistor_step_overshoot", "standalone_step",
                                         true, 15.7,
                                         step_measured, reports[3].max_overshoot_mv,
                                         "mV",
                                         "Model step phase uses total load 506mA->330mA");
        write_comparison_metric_optional("energy_saving_min", "overall_ai",
                                         true, 21.1, false, 0.0, "%",
                                         "Requires paired runs against --disable-learning or --disable-eec");
        write_comparison_metric_optional("energy_saving_max", "overall_ai",
                                         true, 33.3, false, 0.0, "%",
                                         "Requires paired runs against --disable-learning or --disable-eec");
        write_comparison_metric_optional("performance_improvement_min", "overall_ai",
                                         true, 12.3, false, 0.0, "%",
                                         "Requires a DVFS/performance model");
        write_comparison_metric_optional("performance_improvement_max", "overall_ai",
                                         true, 22.5, false, 0.0, "%",
                                         "Requires a DVFS/performance model");

        if (comparison_csv.is_open())
            comparison_csv.close();
    }

    SC_HAS_PROCESS(pdn_paper_monitor);
    pdn_paper_monitor(sc_core::sc_module_name name_,
                      std::size_t num_ldos_ = 9,
                      std::size_t fingers_per_ldo_ = 16,
                      double nominal_vref_ = 0.8,
                      double vin_ = 0.9,
                      unsigned learn_window_cycles_ = 256,
                      unsigned warmup_cycles_ = 0,
                      double eec_latency_bound_ps_ = 200.0)
        : sc_module(name_),
          num_ldos(num_ldos_),
          fingers_per_ldo(fingers_per_ldo_),
          nominal_vref(nominal_vref_),
          vin(vin_),
          learn_window_cycles(learn_window_cycles_),
          warmup_cycles(warmup_cycles_),
          eec_latency_bound_ps(eec_latency_bound_ps_),
          controller_iq_current(460.0e-6),
          eec_enabled(true),
          learning_enabled(true),
          balance_enabled(true),
          clk_sys("clk_sys"),
          rst_n("rst_n"),
          vout("vout", num_ldos_),
          load_current("load_current", num_ldos_),
          activity("activity", num_ldos_),
          finger_current("finger_current", num_ldos_ * fingers_per_ldo_),
          avg_vout("avg_vout"),
          weight_sparsity("weight_sparsity"),
          input_toggle("input_toggle"),
          vrefh("vrefh"),
          vrefl("vrefl"),
          vdrp("vdrp"),
          vos("vos"),
          global_code("global_code"),
          ctie_hi("ctie_hi"),
          ctie_lo("ctie_lo"),
          local_code("local_code", num_ldos_),
          code_sum("code_sum", num_ldos_),
          gate_word("gate_word", num_ldos_),
          balance_adjust("balance_adjust", num_ldos_),
          event_under("event_under", num_ldos_),
          event_over("event_over", num_ldos_),
          event_active("event_active", num_ldos_),
          active_scenario(-1),
          warmup_count(0),
          warmup_done(warmup_cycles_ == 0),
          sample_count(0),
          window_sparsity(0.0),
          window_toggle(0.0),
          prev_eec_valid(false),
          prev_total_load_eec(0.0),
          prev_code_total_eec(0),
          pending_event_latency(false),
          pending_control_latency(false),
          code_total_at_edge(0),
          prev_sys_valid(false),
          prev_total_load_sys(0.0),
          prev_power_valid(false),
          prev_power_code_total(0),
          settle_pending(false),
          settle_left_band(false)
    {
        initialize_reports();
        reset_window();
        open_files();

        SC_METHOD(sys_process);
        sensitive << clk_sys.pos() << rst_n.neg();
        dont_initialize();

        // use for EEC latency measurement; not needed for main learning/monitoring functionality
        // SC_METHOD(eec_process);
        // sensitive << clk_eec.pos() << rst_n.neg();
        // dont_initialize();
    }
};

#endif // PDN_METRICS_H
