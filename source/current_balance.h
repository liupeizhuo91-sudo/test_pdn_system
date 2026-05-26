// current_balance.h
#ifndef CURRENT_BALANCE_H
#define CURRENT_BALANCE_H

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

#include <systemc>

// Workload-driven power-switch balance from Fig. 7 of the paper.
//
// The module runs only in slow mode. It reads every L-LDO local code CLOCAL and
// a normalized workload activity estimate. It emits signed CCB corrections with
// near-zero total sum, so current sharing is improved without changing the
// aggregate local code budget.
SC_MODULE(current_balance) {
    std::size_t num_ldos;
    int max_adjust_code;
    double mismatch_gain;
    double workload_gain;
    bool enabled;

    sc_core::sc_in<bool> clk;
    sc_core::sc_in<bool> rst_n;
    sc_core::sc_in<bool> enable;

    sc_core::sc_vector<sc_core::sc_in<sc_dt::sc_uint<16>>> local_code;
    sc_core::sc_vector<sc_core::sc_in<double>> activity;
    sc_core::sc_vector<sc_core::sc_out<sc_dt::sc_int<17>>> balance_adjust;

    static int clamp_int(int value, int lo, int hi)
    {
        return std::max(lo, std::min(value, hi));
    }

    void write_zero()
    {
        for (std::size_t i = 0; i < num_ldos; ++i)
            balance_adjust[i].write(0);
    }

    void balance_process()
    {
        if (!rst_n.read()) {
            write_zero();
            return;
        }

        if (!enabled || !enable.read()) {
            // During local EEC fast mode the tie-code path should dominate.
            write_zero();
            return;
        }

        double sum_code = 0.0;
        double sum_activity = 0.0;
        for (std::size_t i = 0; i < num_ldos; ++i) {
            sum_code += local_code[i].read().to_uint();
            sum_activity += activity[i].read();
        }

        const double avg_code = sum_code / static_cast<double>(num_ldos);
        const double avg_activity = sum_activity / static_cast<double>(num_ldos);

        std::vector<int> raw(num_ldos, 0);
        int raw_sum = 0;
        for (std::size_t i = 0; i < num_ldos; ++i) {
            const double code_err = avg_code -
                static_cast<double>(local_code[i].read().to_uint());

            // Higher aggregate code means stronger sourcing. Heavier activity
            // should therefore receive a positive code correction.
            const double workload_err = activity[i].read() - avg_activity;
            const double adjust =
                mismatch_gain * code_err +
                workload_gain * workload_err * static_cast<double>(max_adjust_code);

            raw[i] = clamp_int(static_cast<int>(std::lround(adjust)),
                               -max_adjust_code, max_adjust_code);
            raw_sum += raw[i];
        }

        const int mean_raw =
            static_cast<int>(std::lround(static_cast<double>(raw_sum) /
                                         static_cast<double>(num_ldos)));

        for (std::size_t i = 0; i < num_ldos; ++i) {
            const int corrected =
                clamp_int(raw[i] - mean_raw, -max_adjust_code, max_adjust_code);
            balance_adjust[i].write(corrected);
        }
    }

    SC_HAS_PROCESS(current_balance);
    current_balance(sc_core::sc_module_name name_,
                    std::size_t num_ldos_ = 9,
                    int max_adjust_code_ = 512,
                    double mismatch_gain_ = 0.25,
                    double workload_gain_ = 0.50)
        : sc_module(name_),
          num_ldos(num_ldos_),
          max_adjust_code(max_adjust_code_),
          mismatch_gain(mismatch_gain_),
          workload_gain(workload_gain_),
          enabled(true),
          clk("clk"),
          rst_n("rst_n"),
          enable("enable"),
          local_code("local_code", num_ldos_),
          activity("activity", num_ldos_),
          balance_adjust("balance_adjust", num_ldos_)
    {
        SC_METHOD(balance_process);
        sensitive << clk.pos() << rst_n.neg();
        dont_initialize();
    }
};

#endif // CURRENT_BALANCE_H
