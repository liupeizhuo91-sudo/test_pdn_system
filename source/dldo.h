#ifndef _DLDO_H_
#define _DLDO_H_

#include <cstdio>
#include <systemc>
#include <systemc-ams>

#include "pmos_model.h"

// One PMOS finger: gate to vss if ctrl_bit=0, to vdd if ctrl_bit=1 (transmission switches).
SC_MODULE(dldo_leg)
{
    sc_core::sc_in<bool> ctrl_bit;

    sca_eln::sca_terminal vdd;
    sca_eln::sca_terminal vss;
    sca_eln::sca_terminal vout;

    sca_eln::sca_node n_gate_;

    pmos_model pm_;
    sca_eln::sca_de_rswitch sw_gate_vss_;
    sca_eln::sca_de_rswitch sw_gate_vdd_;

    dldo_leg(sc_core::sc_module_name name_, double r_on, double r_off)
        : sc_module(name_),
        ctrl_bit("ctrl_bit"),
        vdd("vdd"),
        vss("vss"),
        vout("vout"),
        n_gate_("n_gate"),
        pm_("pm"),
        sw_gate_vss_("sw_gate_vss", r_on, r_off, true),
        sw_gate_vdd_("sw_gate_vdd", r_on, r_off, false)
    {
        pm_.source(vdd);
        pm_.drain(vout);
        pm_.gate(n_gate_);

        sw_gate_vss_.p(n_gate_);
        sw_gate_vss_.n(vss);
        sw_gate_vss_.ctrl(ctrl_bit);

        sw_gate_vdd_.p(n_gate_);
        sw_gate_vdd_.n(vdd);
        sw_gate_vdd_.ctrl(ctrl_bit);
    }
};

// Digital LDO core: 16 parallel PMOS devices driven by ctrl[15:0].
// Each bit: 0 -> gate tied to vss (device on); 1 -> gate tied to vdd (off).
// Output node vout is decoupled to vss by cout_value (F).

SC_MODULE(dldo)
{
    SC_HAS_PROCESS(dldo);

    sc_core::sc_in<sc_dt::sc_uint<16>> ctrl;

    sca_eln::sca_terminal vdd;
    sca_eln::sca_terminal vss;
    sca_eln::sca_terminal vout;

    /// Output capacitance (farads); set from constructor argument.
    double cout_value;
    double initial_vout;

    sc_core::sc_vector<dldo_leg> leg_;
    sc_core::sc_signal<bool>* gate_ctrl_bit_[16];
    sca_eln::sca_c* cout_cap_;

    explicit dldo(sc_core::sc_module_name name_,
                  double cout_value_ = 1e-9,
                  double initial_vout_ = 0.8)
        : sc_module(name_),
        ctrl("ctrl"),
        vdd("vdd"),
        vss("vss"),
        vout("vout"),
        cout_value(cout_value_),
        initial_vout(initial_vout_),
        leg_("leg"),
        cout_cap_(nullptr)
    {
        init_core(cout_value_, initial_vout_);
    }

    void end_of_elaboration() override { refresh_ctrl_bits(); }

    void refresh_ctrl_bits()
    {
        const sc_dt::sc_uint<16> c = ctrl.read();
        for (int i = 0; i < 16; ++i)
            gate_ctrl_bit_[i]->write(static_cast<bool>(c[i]));
    }

private:
    void init_core(double c_out, double initial_vout_value)
    {
        cout_value = c_out;
        initial_vout = initial_vout_value;

        SC_METHOD(do_refresh_ctrl);
        sensitive << ctrl;
        dont_initialize();

        constexpr double r_on  = 1e-3;
        constexpr double r_off = 1e12;

        char name_buf[64];

        cout_cap_ = new sca_eln::sca_c("cout_cap", c_out,
                                       c_out * initial_vout_value);
        cout_cap_->p(vout);
        cout_cap_->n(vss);

        for (int i = 0; i < 16; ++i)
        {
            std::snprintf(name_buf, sizeof(name_buf), "gate_ctrl_bit_%d", i);
            gate_ctrl_bit_[i] = new sc_core::sc_signal<bool>(name_buf);
        }

        leg_.init(16, [&](const char* name, size_t) {
            return new dldo_leg(name, r_on, r_off);
        });

        for (size_t i = 0; i < 16; ++i)
        {
            leg_[i].vdd(vdd);
            leg_[i].vss(vss);
            leg_[i].vout(vout);
            leg_[i].ctrl_bit(*gate_ctrl_bit_[i]);
        }
    }

    void do_refresh_ctrl() { refresh_ctrl_bits(); }
};

#endif // _DLDO_H_
