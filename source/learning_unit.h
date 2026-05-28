// learning_unit.h
#ifndef LEARNING_UNIT_H
#define LEARNING_UNIT_H

#include <algorithm>
#include <cmath>

#include <systemc>

// Online sparsity-aware droop mitigation from Fig. 5/Fig. 6 of the paper.
//
// The model keeps a small table indexed by runtime weight sparsity and input
// toggle. Each entry learns L-LDO comparator thresholds and EEC tie codes from
// measured droop/overshoot over a window of system-clock cycles.
SC_MODULE(learning_unit) {
    struct Entry {
        double vref_low;
        double vref_high;
        double vdrp;
        double vos;
        sc_dt::sc_uint<16> ctie_hi;
        sc_dt::sc_uint<16> ctie_lo;
        double best_pkpk;
        double best_droop;
        double best_score;
        bool learned;

        Entry()
            : vref_low(0.775),
              vref_high(0.825),
              vdrp(0.740),
              vos(0.860),
              ctie_hi(0xC000),
              ctie_lo(0x1000),
              best_pkpk(1.0),
              best_droop(1.0),
              best_score(1.0),
              learned(false)
        {}
    };

    static const int kBins = 5;
    static const int kTableSize = kBins * kBins;

    sc_core::sc_in<bool> clk;
    sc_core::sc_in<bool> rst_n;
    sc_core::sc_in<double> vout;
    sc_core::sc_in<double> weight_sparsity;
    sc_core::sc_in<double> input_toggle;

    sc_core::sc_out<double> vref;
    sc_core::sc_out<double> vrefh;
    sc_core::sc_out<double> vrefl;
    sc_core::sc_out<double> vdrp;
    sc_core::sc_out<double> vos;
    sc_core::sc_out<sc_dt::sc_uint<16>> ctie_hi;
    sc_core::sc_out<sc_dt::sc_uint<16>> ctie_lo;

    Entry table[kTableSize];

    double nominal_vref;
    double max_window;
    double min_window;
    double learn_step_v;
    unsigned learn_step_code;
    unsigned learn_window_cycles;
    unsigned warmup_cycles;
    bool enabled;

    int active_index;
    unsigned warmup_count;
    unsigned sample_count;
    double min_vout;
    double max_vout;

    static double normalize_ratio(double value)
    {
        const double ratio = value > 1.0 ? value / 100.0 : value;
        return std::max(0.0, std::min(1.0, ratio));
    }

    static int bin5(double value)
    {
        const int bin = static_cast<int>(std::lround(normalize_ratio(value) *
                                                     static_cast<double>(kBins - 1)));
        return std::max(0, std::min(kBins - 1, bin));
    }

    int table_index() const
    {
        return bin5(weight_sparsity.read()) * kBins + bin5(input_toggle.read());
    }

    static double clamp_double(double value, double lo, double hi)
    {
        return std::max(lo, std::min(value, hi));
    }

    static sc_dt::sc_uint<16> add_code(sc_dt::sc_uint<16> value, int delta)
    {
        const int next = static_cast<int>(value.to_uint()) + delta;
        if (next <= 0)
            return sc_dt::sc_uint<16>(0);
        if (next >= 0xFFFF)
            return sc_dt::sc_uint<16>(0xFFFF);
        return sc_dt::sc_uint<16>(next);
    }

    void seed_table()
    {
        for (int s = 0; s < kBins; ++s) {
            for (int t = 0; t < kBins; ++t) {
                Entry& e = table[s * kBins + t];
                const double sparsity = static_cast<double>(s) /
                                        static_cast<double>(kBins - 1);
                const double toggle = static_cast<double>(t) /
                                      static_cast<double>(kBins - 1);
                const double density = (1.0 - sparsity) * (0.5 + 0.5 * toggle);

                const double half_window = 0.010 + 0.025 * density;
                e.vref_low = nominal_vref - half_window;
                e.vref_high = nominal_vref + half_window;
                e.vdrp = nominal_vref - (0.045 + 0.045 * density);
                e.vos = nominal_vref + (0.030 + 0.030 * density);

                // Higher PMOS code means stronger sourcing. Dense/high-toggle
                // bins therefore start with a higher droop tie code, while
                // overshoot recovery starts near the low-code/off side.
                const unsigned hi_code = 0x7000u + static_cast<unsigned>(
                    std::lround(density * static_cast<double>(0x8000u)));
                const unsigned lo_code = 0x0800u + static_cast<unsigned>(
                    std::lround((1.0 - density) * static_cast<double>(0x2000u)));
                e.ctie_hi = sc_dt::sc_uint<16>(
                    std::min<unsigned>(0xFFFFu, hi_code));
                e.ctie_lo = sc_dt::sc_uint<16>(
                    std::min<unsigned>(0xFFFFu, lo_code));
                e.best_pkpk = 1.0;
                e.best_droop = 1.0;
                e.best_score = 1.0;
                e.learned = false;
            }
        }
    }

    void write_entry(const Entry& e)
    {
        vref.write(nominal_vref);
        vrefl.write(e.vref_low);
        vrefh.write(e.vref_high);
        vdrp.write(e.vdrp);
        vos.write(e.vos);
        ctie_hi.write(e.ctie_hi);
        ctie_lo.write(e.ctie_lo);
    }

    Entry disabled_entry() const
    {
        Entry e;
        e.vref_low = nominal_vref - 0.045;
        e.vref_high = nominal_vref + 0.045;
        e.vdrp = nominal_vref - 0.110;
        e.vos = nominal_vref + 0.090;
        e.ctie_hi = sc_dt::sc_uint<16>(0x6800);
        e.ctie_lo = sc_dt::sc_uint<16>(0x1800);
        e.best_pkpk = 1.0;
        e.best_droop = 1.0;
        e.best_score = 1.0;
        e.learned = false;
        return e;
    }

    void write_disabled_entry()
    {
        const Entry e = disabled_entry();
        write_entry(e);
    }

    void reset_window()
    {
        sample_count = 0;
        min_vout = 10.0;
        max_vout = -10.0;
    }

    void enforce_window(Entry& e)
    {
        const double low_bound = nominal_vref - max_window;
        const double high_bound = nominal_vref + max_window;
        const double half_min = min_window * 0.5;

        e.vref_low = clamp_double(e.vref_low, low_bound, nominal_vref - half_min);
        e.vref_high = clamp_double(e.vref_high, nominal_vref + half_min, high_bound);

        if ((e.vref_high - e.vref_low) < min_window) {
            e.vref_low = nominal_vref - half_min;
            e.vref_high = nominal_vref + half_min;
        }
    }

    void adapt_entry(Entry& e)
    {
        const double sparsity = normalize_ratio(weight_sparsity.read());
        const double toggle = normalize_ratio(input_toggle.read());
        const double density = (1.0 - sparsity) * (0.5 + 0.5 * toggle);

        const double droop = std::max(0.0, nominal_vref - min_vout);
        const double overshoot = std::max(0.0, max_vout - nominal_vref);
        const double pkpk = droop + overshoot;

        const double target_droop = 0.012 + 0.030 * density;
        const double target_overshoot = 0.010 + 0.015 * density;
        const double v_step = learn_step_v * (0.5 + density);
        const int code_step = static_cast<int>(
            learn_step_code * (1u + static_cast<unsigned>(std::lround(3.0 * density))));

        const double droop_excess = std::max(0.0, droop - target_droop);
        const double overshoot_excess = std::max(0.0, overshoot - target_overshoot);
        const double score = pkpk + 2.0 * droop_excess + overshoot_excess;

        const bool was_learned = e.learned;
        const bool droop_regressed =
            was_learned && droop > target_droop &&
            droop > e.best_droop + 0.012;
        const bool pkpk_regressed =
            was_learned && pkpk > e.best_pkpk + 0.018 &&
            score > e.best_score + 0.012;
        const bool rollback = pkpk_regressed && !droop_regressed;

        if (!was_learned || score < e.best_score) {
            e.best_score = score;
            e.best_pkpk = pkpk;
            e.best_droop = droop;
            e.learned = true;
        } else {
            if (pkpk < e.best_pkpk)
                e.best_pkpk = pkpk;
            if (droop < e.best_droop && score <= e.best_score + 0.010)
                e.best_droop = droop;
            e.learned = true;
        }

        if (rollback) {
            // Roll back only when peak-to-peak worsens without a droop need.
            e.vref_low -= 2.0 * v_step;
            e.vref_high += 2.0 * v_step;
            e.ctie_hi = add_code(e.ctie_hi, -code_step);
            e.ctie_lo = add_code(e.ctie_lo, code_step);
        } else {
            if (droop > target_droop) {
                const double droop_gain = droop_regressed ? 1.5 : 1.0;
                e.vref_low += droop_gain * v_step;
                e.ctie_hi = add_code(
                    e.ctie_hi,
                    static_cast<int>(std::lround(droop_gain * code_step)));
            } else {
                e.vref_low -= 0.25 * v_step;
                e.ctie_hi = add_code(e.ctie_hi, -code_step / 4);
            }

            if (overshoot > target_overshoot) {
                e.vref_high -= v_step;
                e.ctie_lo = add_code(e.ctie_lo, -code_step);
            } else {
                e.vref_high += 0.25 * v_step;
                e.ctie_lo = add_code(e.ctie_lo, code_step / 4);
            }
        }

        enforce_window(e);
        e.vdrp = clamp_double(nominal_vref - std::max(target_droop, droop),
                              nominal_vref - 0.160, nominal_vref - 0.004);
        e.vos = clamp_double(nominal_vref + std::max(target_overshoot, overshoot),
                             nominal_vref + 0.004, nominal_vref + 0.160);
    }

    void learn_process()
    {
        if (!rst_n.read()) {
            seed_table();
            active_index = table_index();
            warmup_count = 0;
            reset_window();
            if (enabled)
                write_entry(table[active_index]);
            else
                write_disabled_entry();
            return;
        }

        if (!enabled) {
            reset_window();
            write_disabled_entry();
            return;
        }

        const int next_index = table_index();
        if (next_index != active_index) {
            active_index = next_index;
            reset_window();
            write_entry(table[active_index]);
            return;
        }

        if (warmup_count < warmup_cycles) {
            ++warmup_count;
            reset_window();
            write_entry(table[active_index]);
            return;
        }

        const double sample = vout.read();
        min_vout = std::min(min_vout, sample);
        max_vout = std::max(max_vout, sample);
        ++sample_count;

        if (sample_count >= learn_window_cycles) {
            adapt_entry(table[active_index]);
            reset_window();
        }

        write_entry(table[active_index]);
    }

    SC_HAS_PROCESS(learning_unit);
    learning_unit(sc_core::sc_module_name name_,
                  double nominal_vref_ = 0.8,
                  unsigned learn_window_cycles_ = 256,
                  double learn_step_v_ = 1.0e-3,
                  unsigned learn_step_code_ = 32,
                  unsigned warmup_cycles_ = 0)
        : sc_module(name_),
          clk("clk"),
          rst_n("rst_n"),
          vout("vout"),
          weight_sparsity("weight_sparsity"),
          input_toggle("input_toggle"),
          vref("vref"),
          vrefh("vrefh"),
          vrefl("vrefl"),
          vdrp("vdrp"),
          vos("vos"),
          ctie_hi("ctie_hi"),
          ctie_lo("ctie_lo"),
          nominal_vref(nominal_vref_),
          max_window(0.080),
          min_window(0.006),
          learn_step_v(learn_step_v_),
          learn_step_code(learn_step_code_),
          learn_window_cycles(learn_window_cycles_),
          warmup_cycles(warmup_cycles_),
          enabled(true),
          active_index(0),
          warmup_count(0),
          sample_count(0),
          min_vout(10.0),
          max_vout(-10.0)
    {
        seed_table();

        SC_METHOD(learn_process);
        sensitive << clk.pos() << rst_n.neg();
        dont_initialize();
    }
};

#endif // LEARNING_UNIT_H
