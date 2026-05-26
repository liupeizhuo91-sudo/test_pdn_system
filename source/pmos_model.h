#ifndef _PMOS_MODEL_H_
#define _PMOS_MODEL_H_

#include <systemc-ams>
#include <systemc>
#include <cmath>

// Behavioural PMOS model with ELN gate/source/drain terminals.
// Includes cutoff, linear and saturation regions with channel-length modulation
// and a simple velocity saturation limit on Vsd.
//
// Saturation region (using effective Vsd, Vsg):
//   Id = μp * Cox * W / (2L) * (Vsg - |Vt|)^2 * (1 + λ Vsd)
//
// Linear (triode) region:
//   Id = μp * Cox * W / L * [ (Vsg - |Vt|) * Vsd - 0.5 * Vsd^2 ]
//
// Id is defined positive from source to drain (conventional PMOS current).

SC_MODULE(pmos_model)
{
    // ELN terminals
    sca_eln::sca_terminal gate;
    sca_eln::sca_terminal source;
    sca_eln::sca_terminal drain;

    // Internal: measure Vsg and Vsd
    sc_core::sc_signal<double> vsg_sig;
    sc_core::sc_signal<double> vsd_sig;

    sca_eln::sca_de_vsink vs_vsg;
    sca_eln::sca_de_vsink vs_vsd;

    // Controlled current source Ips (from source to drain)
    sc_core::sc_signal<double> ids_sig;      // positive ==> source -> drain
    sca_eln::sca_de_isource ids_src;

    // Expose Ids for testbench access
    sc_core::sc_signal<double>& ids() { return ids_sig; }

    // Device parameters (approx. 40nm technology, scaled for behavioural use)
    double vth;     // |threshold| voltage (V), Vt ~ 0.35-0.4 for 40nm
    double mu;      // hole mobility (m^2/Vs), degraded @40nm (~0.02)
    double cox;     // oxide capacitance per unit area (F/m^2)
    double w;       // channel width (m)
    double l;       // channel length (m)
    double lambda;  // channel-length modulation (1/V)
    double vsat;    // saturation velocity (m/s)

    pmos_model(sc_core::sc_module_name name,
               double vth_    = 0.4,
               double mu_     = 0.02,      // effective hole mobility
               double cox_    = 2.0e-2,   // ~40nm tox => high Cox
               double w_      = 10e-6,    // 10 µm width
               double l_      = 40e-9,    // 40 nm length
               double lambda_ = 0.05,
               double vsat_   = 1e5)
    : sc_module(name),
      vs_vsg("vs_vsg", 1.0),
      vs_vsd("vs_vsd", 1.0),
      ids_src("ids_src", 1.0),
      vth(vth_),
      mu(mu_),
      cox(cox_),
      w(w_),
      l(l_),
      lambda(lambda_),
      vsat(vsat_)
    {
        SC_HAS_PROCESS(pmos_model);

        // Measure Vsg = V(source) - V(gate)
        vs_vsg.p(source);
        vs_vsg.n(gate);
        vs_vsg.outp(vsg_sig);

        // Measure Vsd = V(source) - V(drain)
        vs_vsd.p(source);
        vs_vsd.n(drain);
        vs_vsd.outp(vsd_sig);

        // Current source from source to drain, controlled by ids_sig (A)
        ids_src.p(source);
        ids_src.n(drain);
        ids_src.inp(ids_sig);

        SC_METHOD(update_ids);
        sensitive << vsg_sig << vsd_sig;
        dont_initialize();
    }

    void update_ids()
    {
        double vsg = vsg_sig.read(); // V(source) - V(gate)
        double vsd = vsd_sig.read(); // V(source) - V(drain)

        double ids = 0.0; // positive => source to drain

        if (vsg <= vth || vsd <= 0.0)
        {
            // Cutoff or reverse bias
            ids = 0.0;
        }
        else
        {
            double vov = vsg - vth;      // overdrive
            if (vov < 0.0) vov = 0.0;

            double beta = mu * cox * (w / l); // transconductance parameter

            // Velocity saturation: Esat = vsat / mu, Vdsat_vel = Esat * L
            double esat = vsat / mu;
            double vsd_sat_vel = esat * l;

            // Classical saturation voltage Vsd_sat_classic = Vov
            double vsd_sat_classic = vov;

            // Effective saturation voltage limited by velocity saturation
            double vsd_sat_eff = std::min(vsd_sat_classic, vsd_sat_vel);

            if (vsd < vsd_sat_eff)
            {
                // Linear (triode) region:
                // Id = μp Cox W / L [ (Vsg - Vt) Vsd - 0.5 Vsd^2 ]
                ids = beta * (vov * vsd - 0.5 * vsd * vsd);
            }
            else
            {
                // Saturation with Vsd_sat limited by velocity saturation.
                // Use the triode current at the saturation boundary so the
                // behavioral device does not lose drive strength abruptly.
                double ids_sat =
                    beta * (vov * vsd_sat_eff -
                            0.5 * vsd_sat_eff * vsd_sat_eff);
                ids = ids_sat * (1.0 + lambda * (vsd - vsd_sat_eff));
            }
        }

        ids_sig.write(ids);
    }
};

#endif // _PMOS_MODEL_H_
