#ifndef _CONST_SRC_H_
#define _CONST_SRC_H_

#include <systemc-ams>

SCA_TDF_MODULE(const_src)
{
  sca_tdf::sca_out<double> outp;

  void set_attributes() {
    set_timestep(sc_core::sc_time(100, sc_core::SC_PS));
    outp.set_delay(1);
  }

  void initialize(){ outp.initialize(value); }

  void processing(){ outp.write(value); }

  SCA_CTOR(const_src): outp("outp"){
    accept_attribute_changes();
    value = 0.0;}

  double value;
};

#endif // _CONST_SRC_H_
