// g_ldo.h
#ifndef G_LDO_H
#define G_LDO_H

#include <cstddef>
#include <systemc>
#include <systemc-ams>

#include "comparator.h"
#include "Integrator.h"

SC_MODULE(g_ldo) {
    double ki;            // Integral gain, scaled to the 16-bit control range.
    std::size_t num_ldos; // Number of local LDO enable inputs.
    bool require_all_ldos_enabled;

    // Digital side ports.
    sc_core::sc_vector<sc_core::sc_in<bool>> ldo_en;
    sc_core::sc_in<bool> clk_sys;
    sc_core::sc_in<bool> rst_n;
    sc_core::sc_out<sc_dt::sc_uint<16>> global_code;

    // TDF voltage monitor ports.
    sca_tdf::sca_in<double> vout;
    sca_tdf::sca_in<double> vref;

    // Internal discrete-event signals.
    sc_core::sc_signal<bool> event_cp{"event_cp"};
    sc_core::sc_signal<bool> last_event_cp{"last_event_cp"};
    sc_core::sc_signal<bool> all_ldos_en{"all_ldos_en"};
    sc_core::sc_signal<sc_dt::sc_uint<16>> global_code_sig{"global_code_sig"};
    sc_core::sc_signal<sc_dt::sc_uint<16>> last_global_code{"last_global_code"};

    comparator comp;
    integrator ctrl_integrator;

    void generate_enable()
    {
        if (!require_all_ldos_enabled) {
            all_ldos_en.write(true);
            return;
        }

        bool all_enabled = true;
        for (std::size_t i = 0; i < ldo_en.size(); ++i) {
            if (!ldo_en[i].read()) {
                all_enabled = false;
                break;
            }
        }
        all_ldos_en.write(all_enabled);
    }

    void reg_last_event_cp()
    {
        if (!rst_n.read())
            last_event_cp.write(false);
        else
            last_event_cp.write(event_cp.read());
    }

    void reg_last_global_code()
    {
        if (!rst_n.read())
            last_global_code.write(0);
        else
            last_global_code.write(global_code_sig.read());
    }

    void drive_global_code()
    {
        global_code.write(global_code_sig.read());
    }

    SC_HAS_PROCESS(g_ldo);
    g_ldo(sc_core::sc_module_name name_,
        std::size_t num_ldos_ = 9,
        double ki_ = 1e-3)
        : sc_module(name_),
        ki(ki_),
        num_ldos(num_ldos_),
        require_all_ldos_enabled(true),
        ldo_en("ldo_en", num_ldos_),
        clk_sys("clk_sys"),
        rst_n("rst_n"),
        global_code("global_code"),
        vout("vout"),
        vref("vref"),
        comp("comp"),
        ctrl_integrator("ctrl_integrator")
    {
        comp.vin(vout);
        comp.vref(vref);
        comp.event(event_cp); // event_cp = 1 when vout < vref.

        ctrl_integrator.clk(clk_sys);
        ctrl_integrator.reset_n(rst_n);
        ctrl_integrator.current_CP(event_cp);
        ctrl_integrator.last_CP(last_event_cp);
        ctrl_integrator.enable(all_ldos_en);
        ctrl_integrator.global_ctrl(global_code_sig);
        ctrl_integrator.last_ctrl(last_global_code);
        ctrl_integrator.k_i = static_cast<unsigned>(ki * 65535.0);

        SC_METHOD(generate_enable);
        for (std::size_t i = 0; i < ldo_en.size(); ++i)
            sensitive << ldo_en[i];

        SC_METHOD(reg_last_event_cp);
        sensitive << clk_sys.pos() << rst_n.neg();
        dont_initialize();

        SC_METHOD(reg_last_global_code);
        sensitive << clk_sys.pos() << rst_n.neg();
        dont_initialize();

        SC_METHOD(drive_global_code);
        sensitive << global_code_sig;
    }
};

#endif // G_LDO_H
