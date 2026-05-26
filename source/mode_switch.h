#ifndef MODE_SWITCH_H
#define MODE_SWITCH_H

#include <systemc>

SC_MODULE(mode_switch)
{
    sc_core::sc_in<bool> clk_sys;
    sc_core::sc_in<bool> rst_n;

    sc_core::sc_in<bool> vunder;
    sc_core::sc_in<bool> vover;

    sc_core::sc_out<bool> en_slow;
    sc_core::sc_out<bool> event_active;

    const unsigned wait_cycles;
    unsigned cnt;

    SC_HAS_PROCESS(mode_switch);

    mode_switch(sc_core::sc_module_name name,
                unsigned wait_cycles_ = 4)
        : sc_core::sc_module(name),
          wait_cycles(wait_cycles_),
          cnt(0)
    {
        SC_METHOD(update);
        sensitive << clk_sys.pos() << rst_n.neg()
                  << vunder.pos() << vover.pos();
        dont_initialize();
    }

    void update()
    {
        if (!rst_n.read()) {
            cnt = 0;
            en_slow.write(false);
            event_active.write(false);
            return;
        }

        const bool event = vunder.read() || vover.read();
        event_active.write(event);

        if (event) {
            // Any droop or overshoot event resets the counter.
            // Stay in fast mode.
            cnt = 0;
            en_slow.write(false);
            return;
        }

        // No event: count stable system-clock cycles.
        if (cnt < wait_cycles) {
            cnt++;
            en_slow.write(false);
        } else {
            en_slow.write(true);
        }
    }
};

#endif