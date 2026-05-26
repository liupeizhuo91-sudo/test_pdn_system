// l_ldo.h
#ifndef L_LDO_H
#define L_LDO_H

#include <systemc>
#include <systemc-ams>

#include "comparator.h"
#include "eec.h"
#include "dldo.h"
#include "mode_switch.h"
SC_MODULE(l_ldo) {
    // Digital side ports.
    sc_core::sc_in<bool> clk_sys;
    sc_core::sc_in<bool> rst_n;
    sc_core::sc_in<sc_dt::sc_uint<16>> global_code;
    sc_core::sc_in<sc_dt::sc_uint<16>> ctie_hi_code;
    sc_core::sc_in<sc_dt::sc_uint<16>> ctie_lo_code;
    sc_core::sc_in<sc_dt::sc_int<17>> balance_code;
    sc_core::sc_out<bool> en_slow;

    // Analog ELN terminals
    sca_eln::sca_terminal vdd;
    sca_eln::sca_terminal vss;
    sca_eln::sca_terminal vout;

    // TDF reference voltage inputs.
    sca_tdf::sca_in<double> vref_low;
    sca_tdf::sca_in<double> vref_high;

    sc_core::sc_signal<sc_dt::sc_uint<16>> local_code{"local_code"};
    sc_core::sc_signal<sc_dt::sc_uint<16>> code_sum{"code_sum"};
    sc_core::sc_signal<sc_dt::sc_uint<16>> gate_word{"gate_word"};
    sc_core::sc_signal<sc_dt::sc_uint<16>> last_ctrl_sig{"last_ctrl_sig"};
    sc_core::sc_signal<bool> en_slow_sig{"en_slow_sig"};
    sc_core::sc_signal<bool> mode_event_active{"mode_event_active"};
    sc_core::sc_signal<bool> clk_eec{"clk_eec"};
    sca_eln::sca_tdf::sca_vsink vsink{"vsink"};
    sca_tdf::sca_signal<double> vout_tdf{"vout_tdf"};

    sc_core::sc_signal<bool> event_under_raw{"event_under_raw"};
    sc_core::sc_signal<bool> event_over_raw{"event_over_raw"};
    sc_core::sc_signal<bool> event_under_d{"event_under_d"};
    sc_core::sc_signal<bool> event_over_d{"event_over_d"};
    static const unsigned kStartupCode = 0x4000u;
    static const unsigned kNoEecLocalBias = 0x4000u;

    comparator comp_under{"comp_under"};
    comparator comp_over{"comp_over"};
    eec eec_ctrl{"eec_ctrl"};
    dldo dldo_core;
    mode_switch mode_sw{"mode_sw", 4}; 

    bool eec_enabled;

    static sc_dt::sc_uint<16> sat_add_signed(sc_dt::sc_uint<16> num_unsigned,
                                             sc_dt::sc_int<17> num_signed)
    {
        const long value =
            static_cast<long>(num_unsigned.to_uint()) +
            static_cast<long>(num_signed.to_int());

        if (value <= 0)
            return sc_dt::sc_uint<16>(0);

        if (value >= 0x10000)
            return sc_dt::sc_uint<16>(0xFFFF);

        return sc_dt::sc_uint<16>(static_cast<unsigned>(value));
    }

    static sc_dt::sc_uint<16> sat_add_unsigned(sc_dt::sc_uint<16> lhs,
                                               sc_dt::sc_uint<16> rhs)
    {
        const unsigned value = lhs.to_uint() + rhs.to_uint();
        return value > 0xFFFFu ? sc_dt::sc_uint<16>(0xFFFFu)
                               : sc_dt::sc_uint<16>(value);
    }

    
    void sample_events()
    {
        if (!rst_n.read()) {
            event_under_d.write(false);
            event_over_d.write(false);
            return;
        }

        if (!eec_enabled) {
            event_under_d.write(false);
            event_over_d.write(false);
            return;
        }

        event_under_d.write(event_under_raw.read());
        event_over_d.write(event_over_raw.read());
    }
    
    void reg_last_ctrl()
    {
        if (!rst_n.read())
            last_ctrl_sig.write(kStartupCode);
        else
            last_ctrl_sig.write(sat_add_signed(local_code.read(), balance_code.read()));
    }

    void mux_control_code()
    {
        if (!rst_n.read()) {
            code_sum.write(kStartupCode);
            return;
        }

        if (!eec_enabled) {
            code_sum.write(sat_add_unsigned(
                global_code.read(),
                sc_dt::sc_uint<16>(kNoEecLocalBias)));
            return;
        }

        const bool under = event_under_d.read();
        const bool over = event_over_d.read();

        if (under && !over) {
            // Paper CTieHi: fastest droop response. Higher code turns on
            // more PMOS fingers through code_to_gate_word().
            code_sum.write(ctie_hi_code.read());
        } else if (!under && over) {
            // Paper CTieLo: fastest overshoot recovery. Lower code turns
            // off more fingers.
            code_sum.write(ctie_lo_code.read());
        } else {
            code_sum.write(sat_add_unsigned(global_code.read(), last_ctrl_sig.read()));
        }
    }

    bool drive_event_clock() const
    {
        return event_under_raw.read() || event_over_raw.read();
    }
    void mux_clk()
    {
        if (!rst_n.read()) {
            clk_eec.write(false);
            return;
        }
        if (!eec_enabled) {
            clk_eec.write(false);
        } else if (en_slow_sig.read()) {
            // slow mode --> system clock
            clk_eec.write(clk_sys.read());
        } else {
            clk_eec.write(drive_event_clock() && clk_sys.read());
        }
    }
    // 输出en_slow信号
    void drive_en_slow_out()
    {
        en_slow.write(!eec_enabled || en_slow_sig.read());
    }
    static sc_dt::sc_uint<16> code_to_gate_word(sc_dt::sc_uint<16> code)
    {
        const unsigned raw = code.to_uint();
        const unsigned on_count =
            raw == 0u ? 0u : ((raw * 16u + 0xFFFFu) >> 16);

        // PMOS gate_word: 1 = off, 0 = on
        sc_dt::sc_uint<16> word = 0xFFFF;  // 默认全关

        for (unsigned i = 0; i < on_count && i < 16u; ++i)
            word[i] = 0;                   // gate 拉低，打开 PMOS
        return word;
    }

    void update_gate_word()
    {
        gate_word.write(code_to_gate_word(code_sum.read()));
    }

    SC_HAS_PROCESS(l_ldo);
    l_ldo(sc_core::sc_module_name name_,
        double cout_value_ = 1e-12,
        double initial_vout_ = 0.8)
        : sc_module(name_),
        clk_sys("clk_sys"),
        rst_n("rst_n"),
        global_code("global_code"),
        ctie_hi_code("ctie_hi_code"),
        ctie_lo_code("ctie_lo_code"),
        balance_code("balance_code"),
        vout("vout"),
        vss("vss"),
        en_slow("en_slow"),
        vdd("vdd"),
        vref_low("vref_low"),
        vref_high("vref_high"),
        dldo_core("dldo_core", cout_value_, initial_vout_),
        eec_enabled(true)
    {
        local_code.write(kStartupCode);
        last_ctrl_sig.write(kStartupCode);
        code_sum.write(kStartupCode);
        gate_word.write(code_to_gate_word(sc_dt::sc_uint<16>(kStartupCode)));

        vsink.p(vout);
        vsink.n(vss);
        vsink.outp(vout_tdf);

        comp_under.vin(vout_tdf);
        comp_under.vref(vref_low);
        comp_under.event(event_under_raw); // 1 when vout < VREFL.

        comp_over.vin(vref_high);
        comp_over.vref(vout_tdf);
        comp_over.event(event_over_raw); // 1 when vout > VREFH.

        eec_ctrl.rst_n(rst_n);
        eec_ctrl.clk(clk_eec);
        eec_ctrl.event_under(event_under_raw);
        eec_ctrl.event_over(event_over_raw);
        eec_ctrl.last_ctrl(last_ctrl_sig);
        eec_ctrl.ctrl(local_code);

        mode_sw.clk_sys(clk_sys);
        mode_sw.rst_n(rst_n);
        mode_sw.vunder(event_under_raw);
        mode_sw.vover(event_over_raw);
        mode_sw.en_slow(en_slow_sig);
        mode_sw.event_active(mode_event_active);

        dldo_core.ctrl(gate_word);
        dldo_core.vdd(vdd);
        dldo_core.vss(vss);
        dldo_core.vout(vout);
        SC_METHOD(sample_events);
        sensitive << clk_eec.posedge_event() << rst_n.neg();
        dont_initialize();


        SC_METHOD(reg_last_ctrl);
        sensitive << clk_eec.posedge_event() << rst_n.neg();
        dont_initialize();

        SC_METHOD(mux_control_code);
        sensitive << event_under_d << event_over_d
                  << rst_n << global_code << last_ctrl_sig
                  << ctie_hi_code << ctie_lo_code;

        SC_METHOD(mux_clk);
        sensitive << en_slow_sig << clk_sys << rst_n
                  << event_under_raw << event_over_raw;

        SC_METHOD(update_gate_word);
        sensitive << code_sum;

        SC_METHOD(drive_en_slow_out);
        sensitive << en_slow_sig;
    }
};

#endif // L_LDO_H
