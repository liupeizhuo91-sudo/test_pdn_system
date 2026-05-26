// signal_adapters.h
#ifndef SIGNAL_ADAPTERS_H
#define SIGNAL_ADAPTERS_H

#include <cstddef>

#include <systemc>
#include <systemc-ams>

SCA_TDF_MODULE(de_to_tdf_double) {
    sca_tdf::sca_de::sca_in<double> inp;
    sca_tdf::sca_out<double> outp;

    sc_core::sc_time timestep;
    double initial_value;

    void set_attributes()
    {
        set_timestep(timestep);
        outp.set_delay(1);
    }

    void initialize()
    {
        outp.initialize(initial_value);
    }

    void processing()
    {
        outp.write(inp.read());
    }

    de_to_tdf_double(sc_core::sc_module_name name_,
                     sc_core::sc_time timestep_ =
                         sc_core::sc_time(100.0, sc_core::SC_PS),
                     double initial_value_ = 0.8)
        : inp("inp"),
          outp("outp"),
          timestep(timestep_),
          initial_value(initial_value_)
    {
        accept_attribute_changes();
    }
};

SCA_TDF_MODULE(tdf_average) {
    sc_core::sc_vector<sca_tdf::sca_in<double>> inp;
    sca_tdf::sca_out<double> outp;

    std::size_t count;
    sc_core::sc_time timestep;

    void set_attributes()
    {
        set_timestep(timestep);
    }

    void processing()
    {
        double sum = 0.0;
        for (std::size_t i = 0; i < count; ++i)
            sum += inp[i].read();

        outp.write(count == 0 ? 0.0 : sum / static_cast<double>(count));
    }

    tdf_average(sc_core::sc_module_name name_,
                std::size_t count_,
                sc_core::sc_time timestep_ =
                    sc_core::sc_time(100.0, sc_core::SC_PS))
        : inp("inp", count_),
          outp("outp"),
          count(count_),
          timestep(timestep_)
    {
        accept_attribute_changes();
    }
};

#endif // SIGNAL_ADAPTERS_H
