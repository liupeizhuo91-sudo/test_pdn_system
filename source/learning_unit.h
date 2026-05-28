// learning_unit.h
#ifndef LEARNING_UNIT_H
#define LEARNING_UNIT_H

#include <algorithm>
#include <cmath>
#include <limits>

#include <systemc>

// Online sparsity-aware droop mitigation from Fig. 5/Fig. 6 of the paper.
//
// The model keeps a small table indexed by runtime weight sparsity and input
// toggle. Each entry learns L-LDO comparator thresholds and EEC tie codes from
// measured droop/overshoot over a window of system-clock cycles.
SC_MODULE(learning_unit) {
    enum TunePhase {
        TUNE_VREFL = 0,
        TUNE_VREFH = 1,
        TUNE_CTIE_HI = 2,
        TUNE_CTIE_LO = 3,
        EVAL_DROOP = 4,
        EVAL_OVERSHOOT = 5,
        TUNE_DONE = 6
    };

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
        TunePhase phase;
        unsigned phase_iter;
        int tune_direction;
        bool phase_has_best;
        bool tried_reverse;
        double phase_best_score;
        double phase_best_pkpk;
        double phase_best_droop;
        double phase_best_overshoot;
        double saved_vref_low;
        double saved_vref_high;
        double saved_vdrp;
        double saved_vos;
        sc_dt::sc_uint<16> saved_ctie_hi;
        sc_dt::sc_uint<16> saved_ctie_lo;
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
              phase(TUNE_VREFL),
              phase_iter(0),
              tune_direction(1),
              phase_has_best(false),
              tried_reverse(false),
              phase_best_score(std::numeric_limits<double>::max()),
              phase_best_pkpk(std::numeric_limits<double>::max()),
              phase_best_droop(std::numeric_limits<double>::max()),
              phase_best_overshoot(std::numeric_limits<double>::max()),
              saved_vref_low(0.775),
              saved_vref_high(0.825),
              saved_vdrp(0.740),
              saved_vos(0.860),
              saved_ctie_hi(0xC000),
              saved_ctie_lo(0x1000),
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
    sc_core::sc_out<unsigned> learning_phase;

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

    static unsigned phase_code(TunePhase phase)
    {
        return static_cast<unsigned>(phase);
    }

    static TunePhase next_phase(TunePhase phase)
    {
        switch (phase) {
        case TUNE_VREFL:
            return TUNE_VREFH;
        case TUNE_VREFH:
            return TUNE_CTIE_HI;
        case TUNE_CTIE_HI:
            return TUNE_CTIE_LO;
        case TUNE_CTIE_LO:
            return EVAL_DROOP;
        case EVAL_DROOP:
            return EVAL_OVERSHOOT;
        case EVAL_OVERSHOOT:
        default:
            return TUNE_DONE;
        }
    }

    static bool code_phase(TunePhase phase)
    {
        return phase == TUNE_CTIE_HI || phase == TUNE_CTIE_LO;
    }

    static unsigned max_phase_iters(TunePhase phase)
    {
        switch (phase) {
        case TUNE_VREFL:
            return 5;
        case TUNE_VREFH:
            return 6;
        case TUNE_CTIE_HI:
        case TUNE_CTIE_LO:
            return 8;
        case EVAL_DROOP:
        case EVAL_OVERSHOOT:
            return 1;
        case TUNE_DONE:
        default:
            return 0;
        }
    }

    static double score_value(double droop, double overshoot,
                              double target_droop,
                              double target_overshoot)
    {
        const double pkpk = droop + overshoot;
        const double droop_excess = std::max(0.0, droop - target_droop);
        const double overshoot_excess =
            std::max(0.0, overshoot - target_overshoot);
        return pkpk + 2.0 * droop_excess + overshoot_excess;
    }

    static int phase_direction(TunePhase phase, double droop, double overshoot,
                               double target_droop,
                               double target_overshoot)
    {
        switch (phase) {
        case TUNE_VREFL:
            return 1;
        case TUNE_VREFH:
            return -1;
        case TUNE_CTIE_HI:
            return droop > target_droop ? 1 : -1;
        case TUNE_CTIE_LO:
            return overshoot > target_overshoot ? -1 : 1;
        default:
            return 0;
        }
    }

    static void reset_phase_state(Entry& e, TunePhase phase, int direction)
    {
        e.phase = phase;
        e.phase_iter = 0;
        e.tune_direction = direction;
        e.phase_has_best = false;
        e.tried_reverse = false;
        e.phase_best_score = std::numeric_limits<double>::max();
        e.phase_best_pkpk = std::numeric_limits<double>::max();
        e.phase_best_droop = std::numeric_limits<double>::max();
        e.phase_best_overshoot = std::numeric_limits<double>::max();
    }

    static void reset_learning_state(Entry& e)
    {
        e.best_pkpk = 1.0;
        e.best_droop = 1.0;
        e.best_score = 1.0;
        reset_phase_state(e, TUNE_VREFL, 1);
        e.learned = false;
    }

    static void save_phase_best(Entry& e, double score, double pkpk,
                                double droop, double overshoot)
    {
        e.phase_has_best = true;
        e.phase_best_score = score;
        e.phase_best_pkpk = pkpk;
        e.phase_best_droop = droop;
        e.phase_best_overshoot = overshoot;
        e.saved_vref_low = e.vref_low;
        e.saved_vref_high = e.vref_high;
        e.saved_vdrp = e.vdrp;
        e.saved_vos = e.vos;
        e.saved_ctie_hi = e.ctie_hi;
        e.saved_ctie_lo = e.ctie_lo;
    }

    static void restore_phase_best(Entry& e)
    {
        if (!e.phase_has_best)
            return;

        e.vref_low = e.saved_vref_low;
        e.vref_high = e.saved_vref_high;
        e.vdrp = e.saved_vdrp;
        e.vos = e.saved_vos;
        e.ctie_hi = e.saved_ctie_hi;
        e.ctie_lo = e.saved_ctie_lo;
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
                reset_learning_state(e);
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
        learning_phase.write(phase_code(e.phase));
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
        reset_learning_state(e);
        e.phase = TUNE_DONE;
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

    void update_detector_thresholds(Entry& e, double droop,
                                    double overshoot,
                                    double target_droop,
                                    double target_overshoot)
    {
        e.vdrp = clamp_double(nominal_vref - std::max(target_droop, droop),
                              nominal_vref - 0.160, nominal_vref - 0.004);
        e.vos = clamp_double(nominal_vref + std::max(target_overshoot, overshoot),
                             nominal_vref + 0.004, nominal_vref + 0.160);
    }

    void apply_phase_probe(Entry& e, double v_step, int code_step,
                           double droop, double overshoot,
                           double target_droop,
                           double target_overshoot)
    {
        switch (e.phase) {
        case TUNE_VREFL:
            e.vref_low += v_step;
            break;
        case TUNE_VREFH:
            e.vref_high -= v_step;
            break;
        case TUNE_CTIE_HI:
            e.ctie_hi = add_code(e.ctie_hi, e.tune_direction * code_step);
            break;
        case TUNE_CTIE_LO:
            e.ctie_lo = add_code(e.ctie_lo, e.tune_direction * code_step);
            break;
        case EVAL_DROOP:
        case EVAL_OVERSHOOT:
            update_detector_thresholds(e, droop, overshoot,
                                       target_droop, target_overshoot);
            break;
        case TUNE_DONE:
        default:
            break;
        }

        enforce_window(e);
    }

    void start_next_phase(Entry& e, TunePhase next, double score,
                          double pkpk, double droop, double overshoot,
                          double target_droop, double target_overshoot,
                          double v_step, int code_step)
    {
        reset_phase_state(
            e, next,
            phase_direction(next, droop, overshoot,
                            target_droop, target_overshoot));

        if (next == TUNE_DONE)
            return;

        if (next == EVAL_DROOP || next == EVAL_OVERSHOOT) {
            save_phase_best(e, score, pkpk, droop, overshoot);
            return;
        }

        save_phase_best(e, score, pkpk, droop, overshoot);
        apply_phase_probe(e, v_step, code_step, droop, overshoot,
                          target_droop, target_overshoot);
        ++e.phase_iter;
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
            learn_step_code * (1u + static_cast<unsigned>(
                std::lround(3.0 * density))));
        const double score =
            score_value(droop, overshoot, target_droop, target_overshoot);

        if (!e.learned || score < e.best_score) {
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

        if (e.phase == TUNE_DONE)
            return;

        if (e.phase == EVAL_DROOP) {
            update_detector_thresholds(e, droop, overshoot,
                                       target_droop, target_overshoot);
            start_next_phase(e, EVAL_OVERSHOOT, score, pkpk, droop, overshoot,
                             target_droop, target_overshoot,
                             v_step, code_step);
            return;
        }

        if (e.phase == EVAL_OVERSHOOT) {
            update_detector_thresholds(e, droop, overshoot,
                                       target_droop, target_overshoot);
            start_next_phase(e, TUNE_DONE, score, pkpk, droop, overshoot,
                             target_droop, target_overshoot,
                             v_step, code_step);
            return;
        }

        const double improve_margin = 0.0005;
        const bool improved =
            !e.phase_has_best || score < e.phase_best_score - improve_margin;
        if (improved)
            save_phase_best(e, score, pkpk, droop, overshoot);

        const bool worse =
            e.phase_has_best && !improved &&
            score > e.phase_best_score + improve_margin;

        if (worse && code_phase(e.phase) && !e.tried_reverse &&
            e.phase_iter > 0) {
            restore_phase_best(e);
            e.tune_direction = -e.tune_direction;
            e.tried_reverse = true;
            apply_phase_probe(e, v_step, code_step, droop, overshoot,
                              target_droop, target_overshoot);
            ++e.phase_iter;
            return;
        }

        const bool maxed =
            max_phase_iters(e.phase) > 0 &&
            e.phase_iter >= max_phase_iters(e.phase);
        const bool phase_complete = (worse && e.phase_iter > 0) || maxed;
        if (phase_complete) {
            const double best_score =
                e.phase_has_best ? e.phase_best_score : score;
            const double best_pkpk =
                e.phase_has_best ? e.phase_best_pkpk : pkpk;
            const double best_droop =
                e.phase_has_best ? e.phase_best_droop : droop;
            const double best_overshoot =
                e.phase_has_best ? e.phase_best_overshoot : overshoot;
            const TunePhase next = next_phase(e.phase);

            restore_phase_best(e);
            start_next_phase(e, next, best_score, best_pkpk,
                             best_droop, best_overshoot,
                             target_droop, target_overshoot,
                             v_step, code_step);
            return;
        }

        apply_phase_probe(e, v_step, code_step, droop, overshoot,
                          target_droop, target_overshoot);
        ++e.phase_iter;
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
          learning_phase("learning_phase"),
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
