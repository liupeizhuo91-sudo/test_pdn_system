// Integrator.h
#ifndef INTEGRATOR_H
#define INTEGRATOR_H

#include <systemc>

SC_MODULE(integrator) {
    sc_core::sc_in<bool> clk;
    sc_core::sc_in<bool> reset_n;
    sc_core::sc_in<bool> current_CP;
    sc_core::sc_in<bool> last_CP;
    sc_core::sc_in<bool> enable;
    sc_core::sc_in<sc_dt::sc_uint<16>> last_ctrl;
    sc_core::sc_out<sc_dt::sc_uint<16>> global_ctrl;

    sc_dt::sc_uint<16> k_i;
    sc_dt::sc_uint<16> cnt;

    static sc_dt::sc_uint<16> sat_add(sc_dt::sc_uint<16> value, unsigned delta)
    {
        const unsigned base = value.to_uint();
        return (base + delta) > 0xFFFFu ? sc_dt::sc_uint<16>(0xFFFFu)
                                        : sc_dt::sc_uint<16>(base + delta);
    }

    static sc_dt::sc_uint<16> sat_sub(sc_dt::sc_uint<16> value, unsigned delta)
    {
        const unsigned base = value.to_uint();
        return delta > base ? sc_dt::sc_uint<16>(0u)
                            : sc_dt::sc_uint<16>(base - delta);
    }

    void integrator_process()
    {
        if (!reset_n.read()) {
            global_ctrl.write(0);
            cnt = 0;
            return;
        }

        const sc_dt::sc_uint<16> prev = last_ctrl.read();
        if (!enable.read()) {
            global_ctrl.write(prev);
            return;
        }

        const unsigned step = k_i.to_uint() == 0 ? 1u : k_i.to_uint();
        const bool cp = current_CP.read();
        const bool cp_last = last_CP.read();

        if (cp == cp_last) {
            cnt = sat_add(cnt, step);
            if (cp) {
                // vout < vref
                global_ctrl.write(sat_add(prev, step));
            } else {
                // vout >= vref
                global_ctrl.write(sat_sub(prev, step));
            }
            return;
        }

        // Four-state delay compensation from the paper: when comparator state
        // changes, apply half of the accumulated code in the new direction.
        const unsigned half_cnt = cnt.to_uint() / 2u;
        if (cp) {
            global_ctrl.write(sat_sub(prev, half_cnt));
        } else {
            global_ctrl.write(sat_add(prev, half_cnt));
        }
        cnt = 0;
    }

    SC_CTOR(integrator)
        : clk("clk"),
          reset_n("reset_n"),
          current_CP("current_CP"),
          last_CP("last_CP"),
          enable("enable"),
          last_ctrl("last_ctrl"),
          global_ctrl("global_ctrl"),
          k_i(64),
          cnt(0)
    {
        SC_METHOD(integrator_process);
        sensitive << clk.pos() << reset_n.neg();
        dont_initialize();
    }
};

#endif // INTEGRATOR_H
