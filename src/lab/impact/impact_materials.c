/* impact_materials.c - material records for impact runs, each value with its source.
 *
 * The strength constants are those their authors fitted; they are not re-fitted here. Where a record combines two
 * sources (strength from one, equation of state from another), both are named. Thermal and elastic constants are
 * handbook values for the named temper. Every record says in `status` what has been checked against its source.
 *
 * Removed on 2026-09-25 before any use: records for Armco iron, 4340 steel and 6061-T6 aluminium typed from memory. The
 * report number given for the aluminium constants turned out to belong to an unrelated report, so none of the three
 * is kept until each is traced to a source that has been read. The 6061-T6 record below was rebuilt on 2026-09-26 from
 * sources that were read, value by value. */
#include "impact_materials.h"

#include <stdio.h>
#include <string.h>

static const ImMaterial LIB[] = {
    {"ofhc_copper", 8960, 46.0e9,
     /* Johnson-Cook */ 90e6, 292e6, 0.31, 0.025, 1.09, 1.0, 294, 1356, 383, 0.9,
     /* Mie-Gruneisen */ 3940, 1.489, 2.02,
     /* erosion */ 0, 0,
     "strength: Johnson and Cook, Proc. 7th Int. Symp. on Ballistics, The Hague, 1983, pp. 541-547 (OFHC copper: A 90 MPa, B 292 MPa, n 0.31, "
     "C 0.025, m 1.09, Tm 1356 K), as reproduced in LANL report LA-UR-22-29965; equation of state: Steinberg, Equation of state and strength "
     "properties of selected materials, LLNL UCRL-MA-106439 (1996), copper C0 0.394 cm/us, S1 1.489, gamma0 2.02; shear modulus 46 GPa, "
     "cp 383 J/(kg K): handbook",
     "Johnson-Cook constants checked against LA-UR-22-29965 on 2026-09-25; equation of state typed from the cited table, not yet re-checked"},
    {"aluminium_6061_t6", 2703, 26.82e9,
     /* Johnson-Cook */ 324e6, 114e6, 0.42, 0.002, 1.34, 1.0, 294, 933.6, 893, 0.9,
     /* Mie-Gruneisen */ 5349, 1.338, 2.01,
     /* erosion */ 0, 0,
     "density 2703 kg/m^3 and shear modulus rho0 cs^2 with the ultrasonic shear speed 3.15 km/s: Marsh (ed.), LASL Shock Hugoniot Data, "
     "University of California Press 1980, pp. 181-182 (aluminum 6061); Hugoniot us = 5.349 + 1.338 up km/s: least-squares fit made here to "
     "the 25 shock states of the same table (up 0.44 to 3.84 km/s, 7 to 108 GPa, rms 0.11 km/s); gamma0 = 2 s - 2/3 (Slater's relation), "
     "not measured; strength: Lesuer, Kay and LeBlanc, LLNL UCRL-JC-134118 (2001), Table 1 (A 324 MPa, B 114 MPa, n 0.42, C 0.002, m 1.34, "
     "rate normalised to 1/s); melting 933.6 K and specific heat 743.13 + 0.51 T J/(kg K) at 294 K: Banerjee and Bhawalkar, J. Mech. Mater. "
     "Struct. 3 (2008) 391, appendix A; room temperature 294 K and heat fraction 0.9 as for the copper record",
     "every value read in the named source on 2026-09-26 (the Marsh table from the Internet Archive scan, the others from their PDFs); gamma0 "
     "derived, not measured"},
};

const ImMaterial *im_material_find(const char *name) {
    for (size_t i = 0; i < sizeof LIB / sizeof LIB[0]; i++)
        if (!strcmp(LIB[i].name, name)) return &LIB[i];
    return NULL;
}

int im_material_count(void) { return (int)(sizeof LIB / sizeof LIB[0]); }
const ImMaterial *im_material_at(int i) { return (i >= 0 && i < im_material_count()) ? &LIB[i] : NULL; }
