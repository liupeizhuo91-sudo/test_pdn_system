// distributed_pdn_system.h
#ifndef DISTRIBUTED_PDN_SYSTEM_H
#define DISTRIBUTED_PDN_SYSTEM_H

#include <algorithm>
#include <cstdio>
#include <cstddef>
#include <vector>

#include <systemc>
#include <systemc-ams>

#include "current_balance.h"
#include "g_ldo.h"
#include "l_ldo.h"
#include "learning_unit.h"
#include "signal_adapters.h"

// System-level model of the VLSI'25 distributed power-management architecture:
// 3x3 DCIM clusters, one local event-driven DLDO per cluster, one global slow
// loop, online sparsity-aware learning, and workload-driven current balance.
SC_MODULE(distributed_pdn_system) {
    static const std::size_t kDefaultLdos = 9;

    std::size_t num_ldos;
    double grid_resistance;
    double local_cout;
    double grid_decap;

    sc_core::sc_in<bool> clk_sys;
    sc_core::sc_in<bool> rst_n;
    sc_core::sc_vector<sc_core::sc_in<double>> load_current;
    sc_core::sc_vector<sc_core::sc_in<double>> activity;
    sc_core::sc_in<double> weight_sparsity;
    sc_core::sc_in<double> input_toggle;

    sca_eln::sca_terminal vdd;
    sca_eln::sca_terminal vss;

    sc_core::sc_vector<sca_eln::sca_node> vout_node;
    sc_core::sc_vector<l_ldo> lldo;
    sc_core::sc_vector<sca_eln::sca_de_isource> load_src;
    sc_core::sc_vector<sca_eln::sca_de_vsink> vout_de_sense;
    sc_core::sc_vector<sca_eln::sca_tdf::sca_vsink> vout_tdf_sense;
    sc_core::sc_vector<sca_eln::sca_c> grid_cap;
    sc_core::sc_vector<sca_eln::sca_r> vout_bleed;
    std::vector<sca_eln::sca_r*> grid_resistors;

    sc_core::sc_vector<sc_core::sc_signal<double>> vout_de;
    sc_core::sc_vector<sca_tdf::sca_signal<double>> vout_tdf;
    sc_core::sc_signal<double> avg_vout_de{"avg_vout_de"};
    sca_tdf::sca_signal<double> avg_vout_tdf{"avg_vout_tdf"};

    sc_core::sc_signal<double> learned_vref{"learned_vref"};
    sc_core::sc_signal<double> learned_vrefh{"learned_vrefh"};
    sc_core::sc_signal<double> learned_vrefl{"learned_vrefl"};
    sc_core::sc_signal<double> learned_vdrp{"learned_vdrp"};
    sc_core::sc_signal<double> learned_vos{"learned_vos"};
    sc_core::sc_signal<sc_dt::sc_uint<16>> learned_ctie_hi{"learned_ctie_hi"};
    sc_core::sc_signal<sc_dt::sc_uint<16>> learned_ctie_lo{"learned_ctie_lo"};

    sca_tdf::sca_signal<double> vref_tdf{"vref_tdf"};
    sca_tdf::sca_signal<double> vrefh_tdf{"vrefh_tdf"};
    sca_tdf::sca_signal<double> vrefl_tdf{"vrefl_tdf"};

    sc_core::sc_vector<sc_core::sc_signal<bool>> slow_enable;
    sc_core::sc_signal<bool> balance_enable{"balance_enable"};
    sc_core::sc_signal<sc_dt::sc_uint<16>> global_code{"global_code"};
    sc_core::sc_vector<sc_core::sc_signal<sc_dt::sc_uint<16>>> global_code_per_ldo;
    sc_core::sc_vector<sc_core::sc_signal<sc_dt::sc_int<17>>> balance_adjust;

    g_ldo global_loop;
    learning_unit learner;
    current_balance balancer;
    tdf_average avg_tdf;
    de_to_tdf_double vref_bridge;
    de_to_tdf_double vrefh_bridge;
    de_to_tdf_double vrefl_bridge;

    static sc_dt::sc_uint<16> add_signed_code(sc_dt::sc_uint<16> base,
                                              sc_dt::sc_int<17> delta)
    {
        const long next = static_cast<long>(base.to_uint()) +
                          static_cast<long>(delta.to_int());
        if (next <= 0)
            return sc_dt::sc_uint<16>(0);
        if (next >= 0xFFFF)
            return sc_dt::sc_uint<16>(0xFFFF);
        return sc_dt::sc_uint<16>(static_cast<unsigned>(next));
    }

    void update_avg_vout_de()
    {
        double sum = 0.0;
        for (std::size_t i = 0; i < num_ldos; ++i)
            sum += vout_de[i].read();

        avg_vout_de.write(num_ldos == 0 ? 0.0 : sum / static_cast<double>(num_ldos));
    }


    void update_global_distribution()
    {
        for (std::size_t i = 0; i < num_ldos; ++i) {
            global_code_per_ldo[i].write(
                add_signed_code(global_code.read(), balance_adjust[i].read()));
        }
    }

    void update_balance_enable()
    {
        bool all_slow = num_ldos > 0;
        for (std::size_t i = 0; i < num_ldos; ++i) {
            if (!slow_enable[i].read()) {
                all_slow = false;
                break;
            }
        }
        balance_enable.write(all_slow);
    }

 

    void add_grid_resistor(std::size_t a, std::size_t b, const char* prefix, int index)
    {
        char name[64];
        std::snprintf(name, sizeof(name), "%s_%d", prefix, index);
        sca_eln::sca_r* resistor = new sca_eln::sca_r(name, grid_resistance);
        resistor->p(vout_node[a]);
        resistor->n(vout_node[b]);
        grid_resistors.push_back(resistor);
    }

    void bind_grid()
    {
        int h_index = 0;
        int v_index = 0;
        for (std::size_t row = 0; row < 3; ++row) {
            for (std::size_t col = 0; col < 3; ++col) {
                const std::size_t idx = row * 3 + col;
                if (col < 2)
                    add_grid_resistor(idx, idx + 1, "r_grid_h", h_index++);
                if (row < 2)
                    add_grid_resistor(idx, idx + 3, "r_grid_v", v_index++);
            }
        }
    }

    void apply_runtime_options(bool enable_eec,
                               bool enable_learning,
                               bool enable_balance)
    {
        learner.enabled = enable_learning;
        balancer.enabled = enable_balance;
        for (std::size_t i = 0; i < num_ldos; ++i)
            lldo[i].eec_enabled = enable_eec;
    }

    SC_HAS_PROCESS(distributed_pdn_system);
    distributed_pdn_system(sc_core::sc_module_name name_,
                           std::size_t num_ldos_ = kDefaultLdos,
                           double nominal_vref_ = 0.8,
                           double grid_resistance_ = 50.0e-3,
                           double local_cout_ = 3.3e-9,
                           double grid_decap_ = 0.5e-9,
                           unsigned learning_warmup_cycles_ = 0)
        : sc_module(name_),
          num_ldos(num_ldos_),
          grid_resistance(grid_resistance_),
          local_cout(local_cout_),
          grid_decap(grid_decap_),
          clk_sys("clk_sys"),
          rst_n("rst_n"),
          load_current("load_current", num_ldos_),
          activity("activity", num_ldos_),
          weight_sparsity("weight_sparsity"),
          input_toggle("input_toggle"),
          vdd("vdd"),
          vss("vss"),
          vout_node("vout_node", num_ldos_),
          lldo("lldo"),
          load_src("load_src", num_ldos_),
          vout_de_sense("vout_de_sense", num_ldos_),
          vout_tdf_sense("vout_tdf_sense", num_ldos_),
          grid_cap("grid_cap"),
          vout_bleed("vout_bleed"),
          vout_de("vout_de", num_ldos_),
          vout_tdf("vout_tdf", num_ldos_),
          slow_enable("slow_enable", num_ldos_),
          global_code_per_ldo("global_code_per_ldo", num_ldos_),
          balance_adjust("balance_adjust", num_ldos_),
          global_loop("global_loop", num_ldos_, 1.0e-3),
          learner("learner", nominal_vref_, 256, 1.0e-3, 32,
                  learning_warmup_cycles_),
          balancer("balancer", num_ldos_, 512, 0.25, 0.50),
          avg_tdf("avg_tdf", num_ldos_, sc_core::sc_time(100.0, sc_core::SC_PS)),
          vref_bridge("vref_bridge", sc_core::sc_time(100.0, sc_core::SC_PS),
                      nominal_vref_),
          vrefh_bridge("vrefh_bridge", sc_core::sc_time(100.0, sc_core::SC_PS),
                       nominal_vref_ + 0.025),
          vrefl_bridge("vrefl_bridge", sc_core::sc_time(100.0, sc_core::SC_PS),
                       nominal_vref_ - 0.025)
    {
        lldo.init(num_ldos, [&](const char* name, std::size_t) {
            return new l_ldo(name, local_cout, nominal_vref_);
        });

        grid_cap.init(num_ldos, [&](const char* name, std::size_t) {
            return new sca_eln::sca_c(name, grid_decap,
                                      sca_util::SCA_UNDEFINED);
        });

        vout_bleed.init(num_ldos, [&](const char* name, std::size_t) {
            return new sca_eln::sca_r(name, 1.0e9);
        });

        bind_grid();

        learner.clk(clk_sys);
        learner.rst_n(rst_n);
        learner.vout(avg_vout_de);
        learner.weight_sparsity(weight_sparsity);
        learner.input_toggle(input_toggle);
        learner.vref(learned_vref);
        learner.vrefh(learned_vrefh);
        learner.vrefl(learned_vrefl);
        learner.vdrp(learned_vdrp);
        learner.vos(learned_vos);
        learner.ctie_hi(learned_ctie_hi);
        learner.ctie_lo(learned_ctie_lo);

        vref_bridge.inp(learned_vref);
        vref_bridge.outp(vref_tdf);
        vrefh_bridge.inp(learned_vrefh);
        vrefh_bridge.outp(vrefh_tdf);
        vrefl_bridge.inp(learned_vrefl);
        vrefl_bridge.outp(vrefl_tdf);

        global_loop.clk_sys(clk_sys);
        global_loop.rst_n(rst_n);
        global_loop.global_code(global_code);
        global_loop.vout(avg_vout_tdf);
        global_loop.vref(vref_tdf);
        global_loop.ldo_en(slow_enable);

        balancer.clk(clk_sys);
        balancer.rst_n(rst_n);
        balancer.enable(balance_enable);

        for (std::size_t i = 0; i < num_ldos; ++i) {
            lldo[i].clk_sys(clk_sys);
            lldo[i].rst_n(rst_n);
            lldo[i].global_code(global_code_per_ldo[i]);
            lldo[i].vdd(vdd);
            lldo[i].vss(vss);
            lldo[i].vout(vout_node[i]);
            lldo[i].vref_low(vrefl_tdf);
            lldo[i].vref_high(vrefh_tdf);
            lldo[i].en_slow(slow_enable[i]);
            lldo[i].balance_code(balance_adjust[i]);
            lldo[i].ctie_hi_code(learned_ctie_hi);
            lldo[i].ctie_lo_code(learned_ctie_lo);
            load_src[i].p(vout_node[i]);
            load_src[i].n(vss);
            load_src[i].inp(load_current[i]);

            grid_cap[i].p(vout_node[i]);
            grid_cap[i].n(vss);

            vout_bleed[i].p(vout_node[i]);
            vout_bleed[i].n(vss);

            vout_de_sense[i].p(vout_node[i]);
            vout_de_sense[i].n(vss);
            vout_de_sense[i].outp(vout_de[i]);

            vout_tdf_sense[i].p(vout_node[i]);
            vout_tdf_sense[i].n(vss);
            vout_tdf_sense[i].outp(vout_tdf[i]);
            avg_tdf.inp[i](vout_tdf[i]);


            balancer.local_code[i](lldo[i].local_code);
            balancer.activity[i](activity[i]);
            balancer.balance_adjust[i](balance_adjust[i]);
        }

        avg_tdf.outp(avg_vout_tdf);

        SC_METHOD(update_avg_vout_de);
        for (std::size_t i = 0; i < num_ldos; ++i)
            sensitive << vout_de[i];


        SC_METHOD(update_global_distribution);
        sensitive << global_code;
        for (std::size_t i = 0; i < num_ldos; ++i)
            sensitive << balance_adjust[i];

        SC_METHOD(update_balance_enable);
        for (std::size_t i = 0; i < num_ldos; ++i)
            sensitive << slow_enable[i];

    }
};

#endif // DISTRIBUTED_PDN_SYSTEM_H
