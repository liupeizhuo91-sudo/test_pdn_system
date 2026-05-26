// dcim_workload.h
#ifndef DCIM_WORKLOAD_H
#define DCIM_WORKLOAD_H

#include <algorithm>
#include <cstddef>

#include <systemc>

// Simple 3x3 DCIM workload source used by the top-level testbench.
// It cycles through dense, medium and sparse transformer-like phases and emits
// both load current and activity metadata for the learning/balance loops.
SC_MODULE(dcim_workload) {
    std::size_t num_clusters;
    double leakage_current;
    double dynamic_current;
    unsigned long long cycle;

    sc_core::sc_in<bool> clk;
    sc_core::sc_in<bool> rst_n;
    sc_core::sc_vector<sc_core::sc_out<double>> load_current;
    sc_core::sc_vector<sc_core::sc_out<double>> activity;
    sc_core::sc_out<double> weight_sparsity;
    sc_core::sc_out<double> input_toggle;

    double spatial_profile(std::size_t i) const
    {
        static const double profile[9] = {
            0.85, 1.10, 0.90,
            0.65, 1.35, 0.75,
            0.80, 1.25, 0.70
        };
        return profile[i % 9];
    }

    void write_phase(double sparsity, double toggle)
    {
        weight_sparsity.write(sparsity);
        input_toggle.write(toggle);

        const bool burst_high = ((cycle / 32ULL) % 2ULL) == 0ULL;
        const double burst = burst_high ? 1.0 : 0.30;
        const double density = std::max(0.0, 1.0 - sparsity) * toggle;

        for (std::size_t i = 0; i < num_clusters; ++i) {
            const double act = std::min(1.0, density * burst * spatial_profile(i));
            activity[i].write(act);
            load_current[i].write(leakage_current + dynamic_current * act);
        }
    }

    void workload_process()
    {
        if (!rst_n.read()) {
            cycle = 0;
            write_phase(0.90, 0.25);
            return;
        }

        ++cycle;
        const unsigned long long phase = (cycle / 4096ULL) % 3ULL;
        if (phase == 0ULL) {
            write_phase(0.10, 0.75); // dense workload
        } else if (phase == 1ULL) {
            write_phase(0.50, 0.50); // medium workload
        } else {
            write_phase(0.90, 0.25); // sparse workload
        }
    }

    SC_HAS_PROCESS(dcim_workload);
    dcim_workload(sc_core::sc_module_name name_,
                  std::size_t num_clusters_ = 9,
                  double leakage_current_ = 1.0e-3,
                  double dynamic_current_ = 18.0e-3)
        : sc_module(name_),
          num_clusters(num_clusters_),
          leakage_current(leakage_current_),
          dynamic_current(dynamic_current_),
          cycle(0),
          clk("clk"),
          rst_n("rst_n"),
          load_current("load_current", num_clusters_),
          activity("activity", num_clusters_),
          weight_sparsity("weight_sparsity"),
          input_toggle("input_toggle")
    {
        SC_METHOD(workload_process);
        sensitive << clk.pos() << rst_n.neg();
        dont_initialize();
    }
};

#endif // DCIM_WORKLOAD_H
