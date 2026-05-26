// eec.h
#ifndef EEC_H
#define EEC_H

#include <systemc>

SC_MODULE(eec) {
    sc_core::sc_in<bool> clk;
    sc_core::sc_in<bool> rst_n;
    sc_core::sc_in<bool> event_under;
    sc_core::sc_in<bool> event_over;
    sc_core::sc_in<sc_dt::sc_uint<16>> last_ctrl;
    sc_core::sc_out<sc_dt::sc_uint<16>> ctrl;

    enum State { IDLE, UNDER, OVER } state;

    unsigned event_count;
    unsigned last_step;
    bool prev_event_under;
    bool prev_event_over;
    sc_dt::sc_uint<16> K_e;
    static const unsigned kStartupCode = 0x4000u;

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

    unsigned adaptive_step(bool same_event)
    {
        if (same_event && event_count < 8)
            ++event_count;
        else if (!same_event)
            event_count = 1;

        const unsigned base = K_e.to_uint() == 0 ? 1u : K_e.to_uint();
        return base << (event_count - 1);
    }

    unsigned reversal_step() const
    {
        const unsigned base = K_e.to_uint() == 0 ? 1u : K_e.to_uint();
        const unsigned half = (last_step + 1u) / 2u;
        return half > base ? half : base;
    }

    void eec_process()
    {
        if (!rst_n.read()) {
            state = IDLE;
            event_count = 0;
            last_step = 0;
            prev_event_under = false;
            prev_event_over = false;
            ctrl.write(kStartupCode);
            return;
        }
        const bool under = event_under.read();
        const bool over = event_over.read();
        const sc_dt::sc_uint<16> base_code = last_ctrl.read();

        if (under && !over) {
            const bool reversing = state == OVER || prev_event_over;
            const unsigned step = reversing ?
                                reversal_step() :
                                adaptive_step(state == UNDER && prev_event_under);
            ctrl.write(sat_add(base_code, step));
            event_count = reversing ? 0u : event_count;
            last_step = step;
            state = UNDER;
        } else if (over && !under) {
            const bool reversing = state == UNDER || prev_event_under;
            const unsigned step = reversing ?
                                reversal_step() :
                                adaptive_step(state == OVER && prev_event_over);
            ctrl.write(sat_sub(base_code, step));
            event_count = reversing ? 0u : event_count;
            last_step = step;
            state = OVER;
        } else {
            event_count = 0;
            last_step = 0;
            ctrl.write(base_code);
            state = IDLE;
        }

        prev_event_under = under;
        prev_event_over = over;
    }

    SC_CTOR(eec)

    :   clk("clk"),
        rst_n("rst_n"),
        event_under("event_under"),
        event_over("event_over"),
        last_ctrl("last_ctrl"),
        ctrl("ctrl"),
        state(IDLE),
        event_count(0),
        last_step(0),
        prev_event_under(false),
        prev_event_over(false),
        K_e(64)
    {
        SC_METHOD(eec_process);
        sensitive << clk.pos() << rst_n.neg();
        dont_initialize();
    }
};

#endif // EEC_H
