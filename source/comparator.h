// comparator.h
#ifndef COMPARATOR_H
#define COMPARATOR_H
#include <systemc-ams>

SCA_TDF_MODULE(comparator) {
    sca_tdf::sca_in<double> vin;       // 来自 VOUT 的电压
    sca_tdf::sca_in<double> vref;      // 参考电压
    sca_tdf::sca_de::sca_out<bool> event;  // 定义为跨域端口

    comparator(sc_core::sc_module_name name_) : vin("vin"), vref("vref"), event("event") {}

    void set_attributes() {
        set_timestep(sc_core::sc_time(100.0, sc_core::SC_PS)); // 100ps 检测间隔
    }

    void processing() {
        event.write(vin.read() < vref.read());
    }
};
#endif