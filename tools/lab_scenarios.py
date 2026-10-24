#!/usr/bin/env python3
"""Write the physics-lab scenarios of the showcase and the flags to examples/lab/*.json.

Each scenario is plain JSON that `build/labrun` runs; shapes that are tedious to type (a capsule profile, a nozzle)
are generated here from their stated dimensions, and the dimensions are recorded in the scenario's "provenance".
Run: python3 tools/lab_scenarios.py
"""
import json
import math
import pathlib

OUT = pathlib.Path(__file__).resolve().parent.parent / 'examples' / 'lab'


def arc(cx, cy, r, a0, a1, n):
    return [[cx + r * math.cos(math.radians(a0 + (a1 - a0) * i / n)), cy + r * math.sin(math.radians(a0 + (a1 - a0) * i / n))]
            for i in range(n + 1)]


def capsule_profile(D=3.912, R=4.694, rs=0.196, cone_deg=33.0, r_top=0.40):
    """Half profile of an Apollo-shaped capsule, heat shield facing -x, closed on the axis.
    Dimensions approximated from published Apollo command module figures (diameter 3.91 m, heat shield
    spherical radius 4.69 m, shoulder radius 0.196 m, afterbody half-angle 33 degrees)."""
    phi_t = math.asin((D / 2 - rs) / (R - rs))
    pts = []
    n = 40
    for i in range(n + 1):  # the spherical heat shield from the axis to the shoulder
        p = phi_t * i / n
        pts.append([R - R * math.cos(p), R * math.sin(p)])
    csx, csy = R - (R - rs) * math.cos(phi_t), (R - rs) * math.sin(phi_t)
    a1 = 180 - math.degrees(phi_t)
    a2 = 90 - cone_deg
    pts += arc(csx, csy, rs, a1, a2, 24)[1:]
    x2, y2 = pts[-1]
    c = math.radians(cone_deg)
    L = (y2 - r_top) / math.sin(c)
    xe = x2 + L * math.cos(c)
    pts.append([xe, r_top])
    pts.append([xe, 0.0])
    return pts, {'diameter_m': D, 'heat_shield_radius_m': R, 'shoulder_radius_m': rs, 'afterbody_half_angle_deg': cone_deg,
                 'top_radius_m': r_top, 'length_m': round(xe, 4)}


# the knobs a person turns in the app's Advanced mode (src/labapp.h, "controls"): the few numbers that change what a
# scenario shows, with a range the solver can take; changing one and pressing RUN WITH THESE makes a new result
def V(path, label, lo, hi, log=False):
    return {'label': label, 'path': path, 'min': lo, 'max': hi, 'log': log}


CONTROLS = {
    'cylinder_': [V('reynolds', 'Reynolds number', 0.5, 200, True)],
    'naca0012_': [V('aoa_deg', 'angle of attack, degrees', 0, 25), V('reynolds', 'Reynolds number', 1000, 10000, True)],
    'capsule_mach6': [V('freestream.mach', 'Mach number', 2, 10)],
    'taylor_copper': [V('bodies.0.velocity_m_s.2', 'impact speed, m/s (downward)', -400, -50)],
    'sphere_plate': [V('bodies.1.velocity_m_s.2', 'sphere speed, m/s (downward)', -1500, -200)],
    'whipple_shield': [V('bodies.0.velocity_m_s.2', 'particle speed, m/s (downward)', -8000, -2000)],
    'crater_aluminium': [V('bodies.0.velocity_m_s.2', 'impact speed, m/s (downward)', -8000, -1000)],
    'chip_cold_plate': [V('boundaries.0.speed_m_s', 'coolant speed, m/s', 0.02, 0.08),
                        V('regions.1.heat_w_m3', 'die heat, W/m^3', 1e8, 1e9, True)],
    'heat_exchanger': [V('boundaries.0.speed_m_s', 'hot water speed, m/s', 0.005, 0.03),
                       V('boundaries.2.speed_m_s', 'cold water speed, m/s', 0.005, 0.03)],
    'data_centre_aisle': [V('boundaries.0.speed_m_s', 'floor supply speed, m/s', 0.3, 2.0),
                          V('regions.0.heat_w_m3', 'rack heat, W/m^3', 1000, 8000)],
    'hvac_room': [V('boundaries.0.speed_m_s', 'supply speed, m/s', 0.3, 1.5), V('boundaries.0.temperature_k', 'supply temperature, K', 285, 295)],
    'micro_mixer': [V('boundaries.0.speed_m_s', 'dye inlet speed, m/s', 1e-4, 2e-3, True),
                    V('boundaries.1.speed_m_s', 'water inlet speed, m/s', 1e-4, 2e-3, True)],
    'seat_climate': [V('boundaries.0.speed_m_s', 'blower speed, m/s', 0.02, 0.2)],
    'crossflow_fan': [V('rotors.0.rpm', 'rotor speed, rpm (clockwise)', -2000, -300)],
    'dam_break': [V('columns.0.box_m.5', 'height of the water column, m', 0.1, 0.55),
                  V('obstacles.0.box_m.5', 'height of the block, m', 0.02, 0.4)],
    'sloshing_tank': [V('sloshing.amplitude_m', 'starting wave amplitude, m', 0.0, 0.2),
                      V('sloshing.depth_m', 'water depth, m', 0.1, 0.7)],
    'loudspeaker': [V('speaker.drive.frequency_hz', 'tone, Hz', 100, 1000, True),
                    V('speaker.drive.voltage_peak_v', 'drive voltage, V peak', 0.5, 20, True)],
    'black_hole': [V('camera.inclination_deg', 'inclination from the axis, deg', 0, 90),
                   V('camera.distance_M', 'camera distance, M', 15, 200, log=True),
                   V('disk.temperature_k', 'disk temperature, K', 2500, 30000, log=True)],
    'pm_motor_drive': [V('currents.peak_density_a_m2', 'current density, A/m^2', 1e6, 1e7),
                       V('drive.load.fan_n_m_s2', 'fan load, N m s^2', 1e-6, 1e-4, log=True)],
    'pm_motor_3d': [V('currents.peak_density_a_m2', 'current density, A/m^2', 0, 1e7)],
    'pm_motor': [V('currents.peak_density_a_m2', 'current density, A/m^2', 0, 1e7),
                 V('currents.electrical_angle_at_zero_deg', 'current angle, electrical degrees', -90, 90)],
}


def write(name, sc):
    for prefix, cs in CONTROLS.items():
        if name.startswith(prefix):
            sc['controls'] = cs
            break
    OUT.mkdir(parents=True, exist_ok=True)
    p = OUT / (name + '.json')
    p.write_text(json.dumps(sc, indent=1) + '\n')
    print('wrote', p.relative_to(OUT.parent.parent))


def capsule(level=4):
    prof, dims = capsule_profile()
    write('capsule_mach6' + ('' if level == 4 else '_l%d' % level), {
        'domain': 'gas',
        'title': 'Apollo-shaped capsule at Mach 6, 30 km',
        'provenance': {
            'shape': 'approximated from published Apollo command module dimensions; a demonstration, not the flown geometry',
            'dimensions': dims,
            'freestream': 'US Standard Atmosphere 1976 at 30 km: 1197 Pa, 226.5 K',
            'physics': 'perfect gas, gamma 1.4, inviscid: real-gas and viscous effects of re-entry are not modelled'},
        'geometry': 'axisymmetric',
        'gas_constant_j_kgk': 287.05,
        'grid': {'x0_m': -3.0, 'y0_m': 0.0, 'length_m': 12.0, 'root_blocks': [8, 4], 'max_level': level,
                 # refine only where neighbouring cells differ by more than 6 per cent: the shock layer behind a Mach 6
                 # bow shock is smooth, and the Loehner indicator alone refined most of it (picture of 2026-09-25)
                 'refine_jump': 0.06},
        'boundaries': {'x_low': 'inflow', 'x_high': 'outflow', 'y_low': 'axis', 'y_high': 'outflow'},
        'freestream': {'mach': 6.0, 'pressure_pa': 1197.0, 'temperature_k': 226.5, 'gamma': 1.4},
        'bodies': [{'polygon_m': [[round(x, 6), round(y, 6)] for x, y in prof]}],
        'run': {'end_time_s': 0.008, 'frames': 64, 'fields': 'rho,p,mach,T,solid', 'cfl': 0.4},
    })


def post_shock(M, rho1, p1, g=1.4):
    """The state behind a normal shock of Mach M moving into gas at rest (Rankine-Hugoniot)."""
    c1 = math.sqrt(g * p1 / rho1)
    rho2 = rho1 * (g + 1) * M * M / ((g - 1) * M * M + 2)
    p2 = p1 * (2 * g * M * M - (g - 1)) / (g + 1)
    u2 = M * c1 * (1 - rho1 / rho2)
    return {'density_kg_m3': rho2, 'velocity_m_s': [u2, 0.0], 'pressure_pa': p2, 'gamma': g}


def double_mach(level=3):
    """A Mach 10 shock meets a 30 degree wedge: the double Mach reflection of Woodward and Colella (1984), set up as
    the physical wedge rather than the rotated frame, in their dimensionless units (rho 1.4, p 1: sound speed 1)."""
    tan30 = math.tan(math.radians(30))
    write('double_mach_reflection', {
        'domain': 'gas', 'title': 'Mach 10 shock on a 30 degree wedge: double Mach reflection',
        'provenance': {'problem': 'Woodward and Colella, J. Comput. Phys. 54 (1984) 115, as a wedge; dimensionless units (rho 1.4, p 1, sound speed 1)'},
        'geometry': 'planar', 'gas_constant_j_kgk': 0,
        'grid': {'x0_m': 0.0, 'y0_m': 0.0, 'length_m': 3.2, 'root_blocks': [16, 10], 'max_level': level},
        'boundaries': {'x_low': 'inflow', 'x_high': 'outflow', 'y_low': 'wall', 'y_high': 'wall'},
        'initial': {'density_kg_m3': 1.4, 'velocity_m_s': [0, 0], 'pressure_pa': 1.0, 'gamma': 1.4},
        'freestream': post_shock(10, 1.4, 1.0),
        'regions': [{'box_m': [-1, -1, 0.3, 3], 'state': post_shock(10, 1.4, 1.0)}],
        'bodies': [{'polygon_m': [[0.5, -0.1], [3.4, -0.1], [3.4, 2.9 * tan30], [0.5, 0.0]]}],
        'run': {'end_time_s': 0.25, 'frames': 50, 'fields': 'rho,p,mach,solid', 'cfl': 0.4},
    })


def shock_helium(level=3):
    """A Mach 1.22 shock in air passes a cylinder of helium: the experiment of Haas and Sturtevant (J. Fluid Mech. 181,
    1987). The helium is compressed, a jet forms along the axis, the cylinder rolls into a pair of vortices. The bubble
    gas is helium with 28 per cent air by mass, the experimenters' estimate, with the mixture's properties of Quirk and
    Karni (ICASE 94-75, table 1); the air ahead of the shock as in So et al. (CTR 2010). Half the tube by symmetry."""
    R_air, R_mix, g_mix = 287.05, 1578.0, 1.648
    rho_air, p0 = 1.20, 101325.0
    T0 = p0 / (rho_air * R_air)
    air = {'density_kg_m3': rho_air, 'velocity_m_s': [0, 0], 'pressure_pa': p0, 'gamma': 1.4}
    he = {'density_kg_m3': p0 / (R_mix * T0), 'velocity_m_s': [0, 0], 'pressure_pa': p0, 'gamma': g_mix}
    write('shock_helium_cylinder', {
        'domain': 'gas', 'title': 'Mach 1.22 shock through a helium cylinder',
        'provenance': {'experiment': 'Haas and Sturtevant, J. Fluid Mech. 181 (1987): cylinder 50 mm, tube 89 mm wide, Mach 1.22',
                       'bubble_gas': 'helium with 28 % air by mass (the experimenters\' estimate): gamma 1.648, R 1578 J/(kg K), '
                                     'Quirk and Karni, ICASE Report 94-75 (1994), table 1',
                       'air': '1.20 kg/m^3 at 101325 Pa ahead of the shock, So et al., CTR Summer Program 2010'},
        'geometry': 'planar', 'gas_constant_j_kgk': R_air,
        'grid': {'x0_m': 0.0, 'y0_m': 0.0, 'length_m': 0.356, 'root_blocks': [16, 2], 'max_level': level},
        'boundaries': {'x_low': 'inflow', 'x_high': 'outflow', 'y_low': 'wall', 'y_high': 'wall'},
        'initial': air,
        'freestream': post_shock(1.22, rho_air, p0),
        'regions': [{'box_m': [-1, -1, 0.02, 1], 'state': post_shock(1.22, rho_air, p0)},
                    {'circle_m': [0.06, 0.0, 0.025], 'state': he}],
        'run': {'end_time_s': 0.0006, 'frames': 60, 'fields': 'rho,p,gamma,vorticity', 'cfl': 0.4},
    })


def nozzle(level=3):
    """A cold-gas thruster: air from a 10 bar reservoir through a conical nozzle (area ratio 3.3) into still air at
    1 bar. Over-expanded: the exit pressure of the design is about 0.4 bar, so oblique shocks and a Mach disk form the
    shock diamonds."""
    t15 = math.tan(math.radians(15))
    r_exit = 0.02 + 0.06 * t15
    wall = [[-0.11, 0.05], [-0.04, 0.05], [-0.02, 0.03], [0.0, 0.02], [0.06, r_exit], [0.06, r_exit + 0.006],
            [0.0, 0.035], [-0.03, 0.2], [-0.11, 0.2]]
    ambient = {'density_kg_m3': 101325.0 / (287.05 * 293.15), 'velocity_m_s': [0, 0], 'pressure_pa': 101325.0, 'gamma': 1.4}
    chamber = {'density_kg_m3': 10e5 / (287.05 * 293.15), 'velocity_m_s': [0, 0], 'pressure_pa': 10e5, 'gamma': 1.4}
    write('thruster_nozzle', {
        'domain': 'gas', 'title': 'Cold-gas nozzle, 10 bar into 1 bar: shock diamonds',
        'provenance': {'shape': 'conical nozzle, throat radius 20 mm, 15 degree divergence, exit radius %.1f mm (area ratio %.2f)' % (1000 * r_exit, (r_exit / 0.02) ** 2)},
        'geometry': 'axisymmetric', 'gas_constant_j_kgk': 287.05,
        'grid': {'x0_m': -0.1, 'y0_m': 0.0, 'length_m': 0.6, 'root_blocks': [16, 4], 'max_level': level},
        'boundaries': {'x_low': 'inflow', 'x_high': 'outflow', 'y_low': 'axis', 'y_high': 'outflow'},
        'initial': ambient,
        'freestream': chamber,
        'regions': [{'box_m': [-1, -1, 0.0, 0.05], 'state': chamber}],
        'bodies': [{'polygon_m': wall}],
        'run': {'end_time_s': 0.0015, 'frames': 60, 'fields': 'rho,p,mach,solid', 'cfl': 0.4},
    })


def shoebox_hall():
    """A shoebox concert hall, 30 x 16 x 12 m, with a stage and 480 upholstered seats: a sound pulse from the stage
    crosses the hall and returns from the walls. Absorption coefficients are typical demonstration values for the
    material classes named, not measured ones."""
    Lx, Ly, Lz = 30.0, 16.0, 12.0
    boxes = [{'box_m': [0, 0, 0, 6.0, Ly, 1.2], 'material': 'wood'}]  # the stage
    for r in range(20):
        x0 = 8.5 + r * 1.0
        for k in range(24):
            y0 = 1.5 + k * 0.55 + (0.8 if k >= 12 else 0.0)  # two blocks of seats with an aisle
            boxes.append({'box_m': [round(x0, 3), round(y0, 3), 0, round(x0 + 0.5, 3), round(y0 + 0.5, 3), 0.8], 'material': 'seat'})
    write('shoebox_hall', {
        'domain': 'acoustic', 'title': 'Shoebox concert hall, 30 x 16 x 12 m: a pulse from the stage',
        'provenance': {'absorption': 'demonstration values by material class (plaster 0.04, ceiling 0.06, wood 0.10, upholstered seat 0.70 at normal incidence), not measured'},
        'room_m': [Lx, Ly, Lz], 'dx_m': 0.1, 'speed_of_sound_m_s': 343.2, 'density_kg_m3': 1.204,
        'materials': [{'name': 'plaster', 'absorption_normal': 0.04}, {'name': 'ceiling', 'absorption_normal': 0.06},
                      {'name': 'wood', 'absorption_normal': 0.10}, {'name': 'seat', 'absorption_normal': 0.70}],
        'walls': {'all': 'plaster', 'z_low': 'wood', 'z_high': 'ceiling'},
        'boxes': boxes,
        'sources': [{'position_m': [3.0, 8.0, 2.7], 'amplitude_pa_at_1m': 1.0, 'pulse_width_s': 4e-4, 'delay_s': 2e-3}],
        'receivers': [{'position_m': [12.0, 5.0, 1.2]}, {'position_m': [20.0, 11.0, 1.2]}, {'position_m': [27.0, 8.0, 1.2]}],
        'run': {'end_time_s': 0.09, 'frames': 45, 'wavefront_threshold_pa': 0.035, 'wavefront_stride': 2},
    })


def taylor(level_cells=(8, 5, 80)):
    """The Taylor anvil test: an OFHC copper cylinder, 25.4 mm long and 7.62 mm across, strikes a rigid anvil at
    190 m/s and mushrooms. Quarter model, drawn whole."""
    write('taylor_copper', {
        'domain': 'impact', 'title': 'Taylor anvil test: OFHC copper at 190 m/s',
        'provenance': {'test': 'the geometry and speed of the classic copper Taylor test (25.4 mm by 7.62 mm, 190 m/s)',
                       'material': 'ofhc_copper, Johnson-Cook constants of Johnson and Cook (1983)'},
        'symmetry': 'quarter', 'anvil_z_m': 0.0,
        'bodies': [{'shape': 'cylinder', 'radius_m': 0.00381, 'z0_m': 0.0, 'length_m': 0.0254, 'cells': list(level_cells),
                    'material': 'ofhc_copper', 'velocity_m_s': [0, 0, -190]}],
        'run': {'end_time_s': 8e-5, 'frames': 40, 'mirror': True},
    })


def sphere_plate():
    """A copper sphere at 800 m/s against a copper plate: the plate dishes, the sphere flattens; erosion at an
    assumed plastic strain of 1.5, stated as such."""
    write('sphere_plate', {
        'domain': 'impact', 'title': 'Copper sphere at 800 m/s against a 4 mm copper plate',
        'provenance': {'erosion': 'fail_strain 1.5 is assumed for the demonstration, not measured'},
        'symmetry': 'quarter',
        'bodies': [{'shape': 'box', 'lo_m': [0, 0, -0.004], 'hi_m': [0.03, 0.03, 0.0], 'cells': [60, 60, 8], 'material': 'ofhc_copper',
                    'velocity_m_s': [0, 0, 0], 'fail_strain': 1.5, 'fail_strain_source': 'assumed for a demonstration'},
                   {'shape': 'sphere', 'center_m': [0, 0, 0.0052], 'radius_m': 0.005, 'cells': 8, 'material': 'ofhc_copper',
                    'velocity_m_s': [0, 0, -800], 'fail_strain': 1.5, 'fail_strain_source': 'assumed for a demonstration'}],
        'run': {'end_time_s': 3e-5, 'frames': 40, 'mirror': True},
    })


def cylinder_photo(re, name, conv, shoulders=False, rake=True, u=0.08, D=24, width=16):
    """The circular cylinder of the classic photographs (Van Dyke, An Album of Fluid Motion): two-dimensional, dye
    released upstream (a rake) or from the cylinder's shoulders (the dye that rolls into the vortex street)."""
    sc = {'domain': 'flow', 'title': 'Circular cylinder at Re %g' % re,
          'provenance': {'photograph': 'Van Dyke, An Album of Fluid Motion (1982): cylinder at Re below 1, at 26, and at 105',
                         'units': 'water, cylinder of 1 cm: the pictures in seconds'},
          'reynolds': re, 'shape': 'cylinder', 'body_cells': D, 'lattice_velocity': u,
          'fluid_kinematic_viscosity_m2_s': 1.0e-6, 'body_size_m': 0.01,
          'tunnel': {'width_d': width, 'length_d': 28, 'upstream_d': 6},
          'dye_every_steps': 2,
          'run': {'convective_times': conv, 'frames': 60, 'film_from_convective_time': conv * 0.5}}
    if shoulders:
        sc['dye_rake'] = {'x_d': 0.0, 'y_from_d': -0.53, 'y_to_d': 0.53, 'count': 2}
    elif rake:
        sc['dye_rake'] = {'x_d': -2.5, 'y_from_d': -1.2, 'y_to_d': 1.2, 'count': 25}
    write(name, sc)


def aerofoil_photo(aoa):
    """NACA 0012 at a chord Reynolds number of 5300, a smoke-wire rake just upstream of the leading edge, as in the
    low-turbulence series of the photographs sent by the owner (angles 0 to 20 degrees)."""
    write('naca0012_re5300_aoa%02d' % aoa, {
        'domain': 'flow', 'title': 'NACA 0012 at Re 5300, %d degrees' % aoa,
        'provenance': {'photograph': 'smoke-wire visualisation of a NACA 0012 at Re_c = 5.3e3, low turbulence (0.6 %), angles 0 to 20 degrees',
                       'model': 'two-dimensional, laminar, no free-stream turbulence'},
        'reynolds': 5300, 'shape': 'naca', 'naca': '0012', 'aoa_deg': aoa, 'body_cells': 160, 'lattice_velocity': 0.06,
        'fluid_kinematic_viscosity_m2_s': 1.5e-5, 'body_size_m': 0.08,
        'tunnel': {'width_d': 4, 'length_d': 7, 'upstream_d': 1.6},
        'dye_rake': {'x_d': -0.45, 'y_from_d': -0.12, 'y_to_d': 0.16, 'count': 12}, 'dye_every_steps': 2,
        'run': {'convective_times': 8, 'frames': 60, 'film_from_convective_time': 4}})


def whipple_shield():
    """A spacecraft shield (Whipple, 1947): a thin aluminium bumper shatters a hypervelocity particle into a cloud that
    spreads before it reaches the rear wall. Demonstration geometry, not a replica of a measured test: 3.2 mm sphere at
    6.5 km/s, 1 mm bumper, 3 mm rear wall 20 mm behind it, all 6061-T6."""
    dx = 0.2e-3
    write('whipple_shield', {
        'domain': 'impact', 'method': 'sph',
        'title': 'Whipple shield: 3.2 mm aluminium sphere at 6.5 km/s',
        'provenance': {'geometry': 'demonstration, chosen for the picture; not a replica of a measured test',
                       'material': 'aluminium_6061_t6 (sources in src/lab/impact/impact_materials.c)'},
        'particle_spacing_m': dx,
        'symmetry': 'quarter',
        'bodies': [{'shape': 'sphere', 'center_m': [0, 0, 0.0016 + dx], 'radius_m': 0.0016, 'material': 'aluminium_6061_t6',
                    'velocity_m_s': [0, 0, -6500]},
                   {'shape': 'box', 'lo_m': [0, 0, -0.001], 'hi_m': [0.012, 0.012, 0.0], 'material': 'aluminium_6061_t6',
                    'velocity_m_s': [0, 0, 0]},
                   {'shape': 'box', 'lo_m': [0, 0, -0.024], 'hi_m': [0.012, 0.012, -0.021], 'material': 'aluminium_6061_t6',
                    'velocity_m_s': [0, 0, 0]}],
        'run': {'end_time_s': 6e-6, 'frames': 60, 'mirror': True},
    })


def crater():
    """Laboratory cratering: an aluminium sphere at 5 km/s into a thick aluminium block, the set-up of the cratering
    experiments that planetary scaling laws are built on. Demonstration geometry."""
    dx = 0.2e-3
    write('crater_aluminium', {
        'domain': 'impact', 'method': 'sph',
        'title': 'Crater: 3.2 mm aluminium sphere at 5 km/s into aluminium',
        'provenance': {'geometry': 'demonstration, chosen for the picture; not a replica of a measured test',
                       'material': 'aluminium_6061_t6 (sources in src/lab/impact/impact_materials.c)'},
        'particle_spacing_m': dx,
        'symmetry': 'quarter',
        'bodies': [{'shape': 'sphere', 'center_m': [0, 0, 0.0016 + dx], 'radius_m': 0.0016, 'material': 'aluminium_6061_t6',
                    'velocity_m_s': [0, 0, -5000]},
                   {'shape': 'box', 'lo_m': [0, 0, -0.008], 'hi_m': [0.010, 0.010, 0.0], 'material': 'aluminium_6061_t6',
                    'velocity_m_s': [0, 0, 0]}],
        'run': {'end_time_s': 1.2e-5, 'frames': 60, 'mirror': True},
    })


# ---- G11 applications on the heat-and-flow solver (src/lab/heat) ---------------------------------------------------
INCROPERA = ('Incropera, DeWitt, Bergman and Lavine, Fundamentals of Heat and Mass Transfer, 6th ed. (2007), %s, 300 K; '
             'typed from the table, not re-checked against a copy in this session')
WATER = {'kinematic_viscosity_m2_s': 855e-6 / 997.0, 'thermal_diffusivity_m2_s': 0.613 / (997.0 * 4179.0),
         'volumetric_heat_capacity_j_m3k': 997.0 * 4179.0, 'expansion_1_k': 276.1e-6, 'density_kg_m3': 997.0,
         'source': INCROPERA % 'table A.6 (saturated water)'}
AIR = {'kinematic_viscosity_m2_s': 15.89e-6, 'thermal_diffusivity_m2_s': 22.5e-6, 'volumetric_heat_capacity_j_m3k': 1.1614 * 1007.0,
       'expansion_1_k': 1 / 300.0, 'density_kg_m3': 1.1614, 'source': INCROPERA % 'table A.4 (air); expansion 1/T of a perfect gas'}
COPPER = {'name': 'copper', 'kind': 'solid', 'conductivity_w_mk': 401.0, 'volumetric_heat_capacity_j_m3k': 8933.0 * 385.0,
          'source': INCROPERA % 'table A.1 (pure copper)'}
SILICON = {'name': 'silicon', 'kind': 'solid', 'conductivity_w_mk': 148.0, 'volumetric_heat_capacity_j_m3k': 2330.0 * 712.0,
           'source': INCROPERA % 'table A.1 (silicon)'}


def chip_cold_plate():
    """A liquid cold plate on a processor, seen in section along the coolant channel: water at 300 K and 0.05 m/s
    through a 3 mm channel over a copper base with ribs, a silicon die underneath dissipating 50 W/cm^2. Demonstration
    geometry, not a product."""
    dx = 1e-4
    nx, ny = 600, 80
    die_q = 50e4 / 1e-3  # 50 W/cm^2 through a 1 mm die: W/m^3
    regions = [{'box_m': [0, 0, nx * dx, 3e-3], 'material': 'copper'},                # base
               {'box_m': [20e-3, 0, 40e-3, 1e-3], 'material': 'silicon', 'heat_w_m3': die_q},  # the die, set into the base
               {'box_m': [0, 6e-3, nx * dx, ny * dx], 'material': 'copper'}]          # lid
    for k in range(12):                                                                # ribs into the channel
        x0 = 12e-3 + k * 3e-3
        regions.append({'box_m': [x0, 3e-3, x0 + 1e-3, 4.5e-3], 'material': 'copper'})
    write('chip_cold_plate', {
        'domain': 'heat', 'title': 'Liquid cold plate: water over copper ribs, a die at 50 W/cm2',
        'provenance': {'geometry': 'demonstration, not a product', 'die': '50 W/cm^2 assumed for the demonstration'},
        'grid': {'nx': nx, 'ny': ny, 'cell_m': dx}, 'fluid': WATER, 'gravity_m_s2': [0, 0],
        'reference_temperature_k': 300.0, 'initial_temperature_k': 300.0, 'largest_speed_m_s': 0.1,
        'materials': [COPPER, SILICON], 'regions': regions,
        'boundaries': [{'side': 'x-', 'from_m': 3e-3, 'to_m': 6e-3, 'type': 'inlet', 'speed_m_s': 0.05, 'parabolic': True, 'temperature_k': 300.0},
                       {'side': 'x+', 'from_m': 3e-3, 'to_m': 6e-3, 'type': 'outlet'}],
        'run': {'end_time_s': 4.0, 'frames': 60},
    })


def data_centre_aisle():
    """A data-centre hall in section: cold air from under a raised floor rises through perforated tiles into the cold
    aisle, server fans draw it through two racks (porous, dissipating 8 kW each over a 2 m by 1 m section per metre of
    depth), and the hot aisle returns it to the ceiling. Demonstration layout."""
    dx = 0.025
    nx, ny = 240, 120  # 6 m by 3 m
    rack = {'name': 'rack', 'kind': 'porous', 'conductivity_w_mk': 0.5, 'volumetric_heat_capacity_j_m3k': 2.0e5, 'permeability_m2': 2e-4,
            'source': 'demonstration values for a rack of servers as a porous heat source, not measured'}
    q = 8000.0 / (2.0 * 1.0)  # W per m^3 over the rack's section, per metre of depth
    write('data_centre_aisle', {
        'domain': 'heat', 'title': 'Data-centre aisle: cold floor supply, two racks, hot ceiling return',
        'provenance': {'layout': 'demonstration', 'racks': 'porous blocks with an assumed permeability and 8 kW each'},
        'grid': {'nx': nx, 'ny': ny, 'cell_m': dx}, 'fluid': AIR, 'gravity_m_s2': [0, -9.81],
        'reference_temperature_k': 295.0, 'initial_temperature_k': 295.0, 'largest_speed_m_s': 2.0,
        'turbulence': {'smagorinsky': 0.16, 'turbulent_prandtl': 0.85},
        'materials': [rack],
        'regions': [{'box_m': [1.5, 0, 2.5, 2.0], 'material': 'rack', 'heat_w_m3': q, 'fan_m_s2': [-2.0, 0]},
                    {'box_m': [3.5, 0, 4.5, 2.0], 'material': 'rack', 'heat_w_m3': q, 'fan_m_s2': [2.0, 0]}],
        'boundaries': [{'side': 'y-', 'from_m': 2.6, 'to_m': 3.4, 'type': 'inlet', 'speed_m_s': 1.0, 'temperature_k': 291.0},
                       {'side': 'y+', 'from_m': 0.3, 'to_m': 0.9, 'type': 'outlet'},
                       {'side': 'y+', 'from_m': 5.1, 'to_m': 5.7, 'type': 'outlet'}],
        'run': {'end_time_s': 120.0, 'frames': 60},
    })


def hvac_room():
    """A ventilated office in section, 5 m by 3 m: a ceiling supply, a floor return, a person and a computer as heat
    sources, a cold window; the passive scalar is the age of the air (seconds since it entered). Demonstration."""
    dx = 0.025
    nx, ny = 200, 120
    write('hvac_room', {
        'domain': 'heat', 'title': 'Ventilated room: supply, return, a person, a window, and the age of the air',
        'provenance': {'layout': 'demonstration', 'loads': 'person 100 W and computer 150 W per metre of depth, assumed'},
        'grid': {'nx': nx, 'ny': ny, 'cell_m': dx}, 'fluid': AIR, 'gravity_m_s2': [0, -9.81],
        'reference_temperature_k': 295.0, 'initial_temperature_k': 295.0, 'largest_speed_m_s': 1.5,
        'turbulence': {'smagorinsky': 0.16, 'turbulent_prandtl': 0.85},
        'scalar': {'diffusivity_m2_s': 22.5e-6, 'initial': 0.0},
        'regions': [{'box_m': [0, 0, nx * dx, ny * dx], 'scalar_source_per_s': 1.0},
                    {'box_m': [2.0, 0, 2.4, 1.7], 'heat_w_m3': 100.0 / (0.4 * 1.7)},
                    {'box_m': [3.2, 0.75, 3.6, 1.1], 'heat_w_m3': 150.0 / (0.4 * 0.35)}],
        'boundaries': [{'side': 'y+', 'from_m': 0.5, 'to_m': 0.8, 'type': 'inlet', 'speed_m_s': 1.0, 'temperature_k': 290.0, 'scalar': 0.0},
                       {'side': 'x+', 'from_m': 0.0, 'to_m': 0.3, 'type': 'outlet'},
                       {'side': 'x-', 'from_m': 1.0, 'to_m': 2.5, 'type': 'wall', 'thermal': 'fixed', 'temperature_k': 283.0}],
        'run': {'end_time_s': 600.0, 'frames': 60},
    })


def micro_mixer():
    """A microfluidic mixer: water with a dye (c = 1) and clear water (c = 0) enter side by side at 1 mm/s in a 1 mm
    channel (Re about 1); the dye spreads only by diffusion (1e-9 m^2/s), so the baffles are what mix it."""
    dx = 5e-6
    nx, ny = 400, 200  # 2 mm by 1 mm
    walls = []
    for k in range(3):  # baffles: alternating from the bottom and the top
        x0 = 0.3e-3 + k * 0.5e-3
        walls.append({'box_m': [x0, 0.0 if k % 2 == 0 else 0.35e-3, x0 + 0.05e-3, 0.65e-3 if k % 2 == 0 else 1.0e-3], 'material': 'glass'})
    glass = {'name': 'glass', 'kind': 'solid', 'conductivity_w_mk': 1.4, 'volumetric_heat_capacity_j_m3k': 2225.0 * 835.0,
             'source': INCROPERA % 'table A.3 (plate glass)'}
    write('micro_mixer', {
        'domain': 'heat', 'title': 'Microfluidic mixer: dye and water around three baffles at Re 1',
        'provenance': {'geometry': 'demonstration', 'diffusivity': '1e-9 m^2/s, the order of small molecules in water, assumed'},
        'grid': {'nx': nx, 'ny': ny, 'cell_m': dx}, 'fluid': WATER, 'gravity_m_s2': [0, 0],
        'reference_temperature_k': 300.0, 'initial_temperature_k': 300.0, 'largest_speed_m_s': 0.002,
        'scalar': {'diffusivity_m2_s': 1e-9, 'initial': 0.0},
        'materials': [glass], 'regions': walls,
        'boundaries': [{'side': 'x-', 'from_m': 0.0, 'to_m': 0.5e-3, 'type': 'inlet', 'speed_m_s': 1e-3, 'parabolic': True, 'temperature_k': 300.0, 'scalar': 1.0},
                       {'side': 'x-', 'from_m': 0.5e-3, 'to_m': 1.0e-3, 'type': 'inlet', 'speed_m_s': 1e-3, 'parabolic': True, 'temperature_k': 300.0, 'scalar': 0.0},
                       {'side': 'x+', 'from_m': 0.0, 'to_m': 1.0e-3, 'type': 'outlet'}],
        'run': {'end_time_s': 6.0, 'frames': 60},
    })


def heat_exchanger():
    """A counter-flow heat exchanger in section: hot water (330 K) flows left through the upper channel, cold water (290
    K) right through the lower one, 2 mm each, separated by a 0.5 mm copper wall. Demonstration geometry."""
    dx = 5e-5
    nx, ny = 1000, 90  # 50 mm by 4.5 mm
    write('heat_exchanger', {
        'domain': 'heat', 'title': 'Counter-flow heat exchanger: hot water over cold through copper',
        'provenance': {'geometry': 'demonstration'},
        'grid': {'nx': nx, 'ny': ny, 'cell_m': dx}, 'fluid': WATER, 'gravity_m_s2': [0, 0],
        'reference_temperature_k': 310.0, 'initial_temperature_k': 310.0, 'largest_speed_m_s': 0.04,
        'materials': [COPPER],
        'regions': [{'box_m': [0, 2.0e-3, nx * dx, 2.5e-3], 'material': 'copper'}],
        'boundaries': [{'side': 'x+', 'from_m': 2.5e-3, 'to_m': 4.5e-3, 'type': 'inlet', 'speed_m_s': 0.02, 'parabolic': True, 'temperature_k': 330.0},
                       {'side': 'x-', 'from_m': 2.5e-3, 'to_m': 4.5e-3, 'type': 'outlet'},
                       {'side': 'x-', 'from_m': 0.0, 'to_m': 2.0e-3, 'type': 'inlet', 'speed_m_s': 0.02, 'parabolic': True, 'temperature_k': 290.0},
                       {'side': 'x+', 'from_m': 0.0, 'to_m': 2.0e-3, 'type': 'outlet'}],
        'run': {'end_time_s': 5.0, 'frames': 50},
    })


def seat_climate():
    """A ventilated seat in section: air at 293 K blown up from a duct through a 10 cm open-cell foam cushion and a 2 cm
    spacer layer under an occupant whose skin is held at 307 K; the air leaves at the front and back. The foam's
    permeability and conductivity are assumed for the demonstration."""
    dx = 2.5e-3
    nx, ny = 200, 48  # 0.5 m by 0.12 m
    foam = {'name': 'foam', 'kind': 'porous', 'conductivity_w_mk': 0.04, 'volumetric_heat_capacity_j_m3k': 5.0e4, 'permeability_m2': 2e-8,
            'source': 'assumed for a demonstration: an open-cell foam, not a measured product'}
    write('seat_climate', {
        'domain': 'heat', 'title': 'Ventilated seat: air through a foam cushion under a warm occupant',
        'provenance': {'geometry': 'demonstration', 'foam': 'permeability 2e-8 m^2 and conductivity 0.04 W/(m K), assumed'},
        'grid': {'nx': nx, 'ny': ny, 'cell_m': dx}, 'fluid': AIR, 'gravity_m_s2': [0, -9.81],
        'reference_temperature_k': 296.0, 'initial_temperature_k': 296.0, 'largest_speed_m_s': 2.0,
        'turbulence': {'smagorinsky': 0.16, 'turbulent_prandtl': 0.85},
        'materials': [foam],
        'regions': [{'box_m': [0.0, 0.0, 0.5, 0.10], 'material': 'foam'}],
        'boundaries': [{'side': 'y-', 'from_m': 0.15, 'to_m': 0.35, 'type': 'inlet', 'speed_m_s': 0.1, 'temperature_k': 293.0},
                       {'side': 'y+', 'from_m': 0.0, 'to_m': 0.5, 'type': 'wall', 'thermal': 'fixed', 'temperature_k': 307.0},
                       {'side': 'x-', 'from_m': 0.0, 'to_m': 0.12, 'type': 'outlet'},
                       {'side': 'x+', 'from_m': 0.0, 'to_m': 0.12, 'type': 'outlet'}],
        'run': {'end_time_s': 20.0, 'frames': 60},
    })


def crossflow_fan():
    """A cross-flow (tangential) fan, the fan of a wall-mounted air conditioner, in section: 24 forward-curved blades on a
    100 mm rotor at 1000 rpm, a rear wall and a tongue; air is drawn in from above and blown out to the side, passing
    through the blade row twice. Demonstration geometry, not a product."""
    dx = 1.5e-3
    nx, ny = 300, 200  # 0.45 m by 0.30 m
    steel = {'name': 'steel', 'kind': 'solid', 'conductivity_w_mk': 60.5, 'volumetric_heat_capacity_j_m3k': 7854.0 * 434.0,
             'source': INCROPERA % 'table A.1 (plain carbon steel)'}
    cx, cy = 0.20, 0.15
    write('crossflow_fan', {
        'domain': 'heat', 'title': 'Cross-flow fan: 24 blades at 1000 rpm, air drawn in above and blown out to the side',
        'provenance': {'geometry': 'demonstration, not a product'},
        'grid': {'nx': nx, 'ny': ny, 'cell_m': dx}, 'fluid': AIR, 'gravity_m_s2': [0, 0],
        'reference_temperature_k': 295.0, 'initial_temperature_k': 295.0, 'largest_speed_m_s': 8.0,
        'turbulence': {'smagorinsky': 0.16, 'turbulent_prandtl': 0.85},
        'materials': [steel],
        'regions': [{'box_m': [0.0, 0.0, 0.14, 0.12], 'material': 'steel'},          # rear wall, below and behind the rotor
                    {'box_m': [0.0, 0.0, 0.45, 0.02], 'material': 'steel'},          # floor of the casing
                    {'box_m': [0.255, 0.155, 0.45, 0.17], 'material': 'steel'}],     # the tongue, beside the rotor
        'rotors': [{'centre_m': [cx, cy], 'rpm': -1000, 'material': 'steel', 'hub_radius_m': 0.0, 'blades': 24,
                    'blade_inner_radius_m': 0.040, 'blade_outer_radius_m': 0.050, 'blade_thickness_m': 0.003, 'blade_angle_deg': 25.0}],
        'boundaries': [{'side': 'y+', 'from_m': 0.05, 'to_m': 0.35, 'type': 'outlet'},
                       {'side': 'x+', 'from_m': 0.02, 'to_m': 0.155, 'type': 'outlet'}],
        'run': {'end_time_s': 0.6, 'frames': 60},
    })


def pm_motor(name='pm_motor', J=5e6, steel_mu=4000.0, drive=False, fan=8.6e-5, three_d=None, drive3d=False):
    """A permanent-magnet motor in cross-section: 12 slots, 4 poles, surface N42 magnets, three-phase distributed
    windings at 5 A/mm^2 turning with the rotor; the rotor swept through one slot pitch gives torque and its ripple.
    Demonstration design, not a product; the steel is linear (no saturation)."""
    dx = 0.25e-3 if not drive else 0.065 / 195  # the running motor on a coarser grid: 131 field solves instead of 31
    N = 520 if not drive else 390
    c = N * dx / 2
    n42 = {'name': 'n42', 'relative_permeability': 1.11, 'remanence_t': 1.315,
           'source': 'Arnold Magnetic Technologies, N42 sintered NdFeB datasheet (rev. 210607): nominal Br 1315 mT and HcB 943 kA/m; '
                     'relative permeability of the straight demagnetisation line through them, Br / (mu0 HcB) = 1.11'}
    steel = {'name': 'steel', 'relative_permeability': steel_mu,
             'source': 'assumed linear lamination steel; with a 1 mm air gap the torque is insensitive to this above about a thousand '
                       '(checked in docs/lab/magnet.md); saturation not modelled'}
    copper = {'name': 'copper', 'relative_permeability': 1.0, 'conductivity_s_m': 5.8e7,
              'source': 'International Annealed Copper Standard: 100 % IACS = 5.8e7 S/m at 20 C'}
    regions = [{'sector_m_deg': [c, c, 0.031, 0.060, 0, 360], 'material': 'steel'}]
    pattern = [('A', 1), ('C', -1), ('B', 1), ('A', -1), ('C', 1), ('B', -1)]
    for k in range(12):
        ph, sg = pattern[k % 6]
        a = 30.0 * k
        regions.append({'sector_m_deg': [c, c, 0.031, 0.048, a - 7.5, a + 7.5], 'material': 'copper', 'phase': ph, 'phase_sign': sg})
    regions.append({'circle_m': [c, c, 0.026], 'material': 'steel', 'rotor': True})
    for k in range(4):
        a = 90.0 * k
        regions.append({'sector_m_deg': [c, c, 0.026, 0.030, a - 40, a + 40], 'material': 'n42', 'magnetisation': 'radial',
                        'magnetisation_angle_deg': 1 if k % 2 == 0 else -1, 'rotor': True})
    regions.append({'circle_m': [c, c, 0.008], 'material': 'air', 'rotor': True})
    if drive:
        aluminium = {'name': 'aluminium', 'relative_permeability': 1.0, 'source': 'aluminium is non-magnetic: relative permeability 1'}
        regions.insert(0, {'sector_m_deg': [c, c, 0.060, 0.064, 0, 360], 'material': 'aluminium'})
        extra_materials = [aluminium]
        drive_block = {
            'source': 'demonstration drive: stack length, fill factor and load are chosen, not taken from a product',
            'stack_length_m': 0.05, 'fill_factor': 0.45, 'rotor_radius_m': 0.030,
            'copper_temperature_coefficient_1_k': 0.00393,
            'copper_temperature_coefficient_source': 'IEC 60028 (1925), International Standard of Resistance for Copper: 0.00393 per K at 20 C',
            'initial_temperature_k': 313.15,
            'torque_table': {'period_deg': 30.0, 'points': 31},
            'load': {'torque_n_m': 0.0, 'fan_n_m_s2': fan, 'source': 'a fan load sized so that the motor settles near 3000 rpm'},
            'spin_up': {'duration_s': 0.5, 'frames': 40, 'step_s': 2e-5},
            'heat_run': {'duration_s': 5400.0, 'frames': 60, 'steps_per_frame': 2},
            'thermal': {
                'air': {'thermal_conductivity_w_mk': 0.0263, 'volumetric_heat_capacity_j_m3k': 1.1614 * 1007.0, 'density_kg_m3': 1.1614,
                        'source': INCROPERA % 'table A.4 (air at 300 K); still air: convection in the gap not modelled'},
                'steel': {'thermal_conductivity_w_mk': 63.9, 'volumetric_heat_capacity_j_m3k': 7832.0 * 434.0, 'density_kg_m3': 7832.0,
                          'source': INCROPERA % 'table A.1 (plain carbon steel AISI 1010), standing in for lamination steel, whose '
                                    'conductivity across the laminations is lower (assumption)'},
                'copper': {'thermal_conductivity_w_mk': 1.0, 'volumetric_heat_capacity_j_m3k': 0.45 * 8933.0 * 385.0, 'density_kg_m3': 0.45 * 8933.0,
                           'source': 'assumed: the equivalent conductivity of an impregnated winding across its wires, about 1 W/(m K) '
                                     '(the range 0.5 to 1.5 reported by Boglietti et al., IEEE Trans. Ind. Electron. 56, 2009); heat '
                                     'capacity of its copper share only (fill factor 0.45, copper from ' + INCROPERA % 'table A.1)'},
                'n42': {'thermal_conductivity_w_mk': 8.5, 'volumetric_heat_capacity_j_m3k': 7600.0 * 450.0, 'density_kg_m3': 7600.0,
                        'source': 'Neorem Magnets, characteristic physical properties of sintered NdFeB at 20 C: density 7.6 g/cm3, specific '
                                  'heat 450 J/(kg K), thermal conductivity 8 to 9 W/(m K) (8.5 used)'},
                'aluminium': {'thermal_conductivity_w_mk': 237.0, 'volumetric_heat_capacity_j_m3k': 2702.0 * 903.0, 'density_kg_m3': 2702.0,
                              'held_temperature_k': 313.15,
                              'source': INCROPERA % 'table A.1 (pure aluminium); the housing is held at 40 C by its cooling water (assumption)'},
            },
        }
    write(name, {
        'domain': 'magnet', 'title': 'Permanent-magnet motor: 12 slots, 4 poles, %g A/mm2' % (J / 1e6),
        'provenance': {'design': 'demonstration, not a product', 'steel': 'linear, relative permeability %g assumed' % steel_mu},
        'grid': {'nx': N, 'ny': N, 'cell_m': dx},
        'materials': [n42, steel, copper] + (extra_materials if drive else []), 'regions': regions,
        'rotor': {'centre_m': [c, c], 'torque_radius_m': 0.0305, 'pole_pairs': 2},
        'currents': {'peak_density_a_m2': J, 'electrical_angle_at_zero_deg': 0.0},
        'run': {'angle_from_deg': 0.0, 'angle_to_deg': 30.0, 'frames': 31},
    } | ({'drive': drive_block, 'title': 'A motor spins up and heats: 12 slots, 4 poles, %g A/mm2' % (J / 1e6),
          'currents': {'peak_density_a_m2': J, 'electrical_angle_at_zero_deg': 180.0}} if drive else {})
      | ({'three_d': three_d, 'title': 'The motor in 3D: 12 slots, 4 poles, 50 mm stack, end windings, %g A/mm2' % (J / 1e6),
          'currents': {'peak_density_a_m2': J, 'electrical_angle_at_zero_deg': 180.0},
          'run': {'angle_from_deg': 0.0, 'angle_to_deg': 30.0, 'frames': 13}} if three_d else {})
      | ({'title': 'The motor in 3D spins up: field, torque and motion, 12 slots, 4 poles, %g A/mm2' % (J / 1e6),
          'drive': {'source': 'demonstration drive: the load is chosen, not taken from a product',
                    'density_kg_m3': {'steel': 7832.0, 'n42': 7600.0,
                                      'source': INCROPERA % 'table A.1 (plain carbon steel AISI 1010) for the steel; Neorem Magnets, '
                                                'sintered NdFeB, 7.6 g/cm3 for the magnets'},
                    'load': {'torque_n_m': 0.0, 'fan_n_m_s2': fan, 'source': 'a fan load sized so that the motor settles near 3000 rpm'},
                    'spin_up': {'duration_s': 0.06, 'frames': 61, 'step_s': 2e-5}}} if drive3d else {}))


def black_hole():
    """Light near a black hole (docs/lab/relativity.md): lengths in the hole's mass M; the disk's temperature is a
    demonstration value that sets its colour."""
    write('black_hole', {
        'domain': 'relativity',
        'title': 'Light bends around a black hole',
        'note': "Lengths in the hole's mass M (G = c = 1): the horizon at 2 M, the photon sphere at 3 M, the innermost "
                "stable orbit at 6 M. The disk is thin and Keplerian with the Shakura-Sunyaev temperature profile; its "
                "colours are those of a blackbody at the temperature each photon brings to the camera (demonstration "
                "scale: temperature_k sets the colour, not a particular object).",
        'camera': {'distance_M': 55, 'inclination_deg': 80, 'azimuth_deg': 0, 'fov_deg': 28},
        'image': {'width': 640, 'height': 360, 'samples_per_axis': 2},
        'disk': {'inner_M': 6, 'outer_M': 20, 'temperature_k': 4000},
        'stars': True,
        'run': {'frames': 60, 'time_from_M': 0, 'time_to_M': 120, 'inclination_from_deg': 72, 'inclination_to_deg': 86},
    })


def dam_break():
    """Water in a tank (docs/lab/water.md): a column of water collapses and its surge runs into a block. The geometry is a
    demonstration in the manner of the SPHERIC dam-break benchmark (Kleefsman et al. 2005), not its dimensions."""
    write('dam_break', {
        'domain': 'water',
        'title': 'A dam breaks and the surge hits a block',
        'note': 'Demonstration geometry (not the SPHERIC Test 2 dimensions). Weakly compressible SPH: see docs/lab/water.md for '
                'what is verified (hydrostatics within 1.65 %, the sloshing period within 0.21 %).',
        'particle_spacing_m': 0.02,
        'gravity_m_s2': 9.80665,
        'water': {'density_kg_m3': WATER['density_kg_m3'], 'source': WATER['source']},
        'tank': {'size_m': [1.6, 0.6, 0.6]},
        'columns': [{'box_m': [1.0, 0.0, 0.0, 1.6, 0.6, 0.4]}],
        'obstacles': [{'box_m': [0.40, 0.22, 0.0, 0.52, 0.38, 0.12]}],
        'run': {'end_s': 2.0, 'frames': 81},
    })


def breaking_wave(dx=0.015):
    """A solitary wave runs up a sloping beach, steepens, plunges and breaks around a pier, in 3D: the wave is uniform
    across the tank until it breaks, and the pier makes the flow three-dimensional (it splits the crest, throws spray and
    leaves a wake in the run-up). Weakly compressible SPH on the GPU."""
    write('breaking_wave', {
        'domain': 'water', 'title': 'A wave breaks on a beach around a pier',
        'note': 'Demonstration geometry: a solitary wave 0.12 m high on 0.25 m of water (H/d = 0.48) meets a 1 in 8 beach and a '
                'cylindrical pier. The initial wave is the Boussinesq solitary wave; see docs/lab/water.md for what is verified.',
        'particle_spacing_m': dx, 'gravity_m_s2': 9.80665,
        'water': {'density_kg_m3': WATER['density_kg_m3'], 'source': WATER['source']},
        'tank': {'size_m': [4.0, 1.0, 0.55]},
        'beach': {'toe_x_m': 1.8, 'slope': 0.125},
        'bodies': [{'shape': 'cylinder', 'centre_m': [2.75, 0.5, 0.275], 'radius_m': 0.06, 'length_m': 0.55, 'axis': 'z'}],
        'solitary_wave': {'height_m': 0.12, 'depth_m': 0.25, 'crest_x_m': 0.9},
        'run': {'end_s': 3.0, 'frames': 91},
    })


METHANE = {'molar_mass_kg_mol': 16.043e-3, 'heat_of_combustion_j_kg': 50.0e6, 'air_per_fuel_kg': 17.2, 'radiative_fraction': 0.2,
           'source': 'methane: molar mass 16.043 g/mol; heat of combustion 50.0 MJ/kg (lower heating value, Drysdale, An '
                     'Introduction to Fire Dynamics, 3rd ed., 2011); 17.2 kg of air per kg from CH4 + 2 (O2 + 3.76 N2); '
                     'radiative fraction 0.2, the value fire models take for methane flames of this size (a demonstration '
                     'value: it is not radiated here, only removed)'}


def methane_fire(Q=100e3, dx=0.04):
    """A methane fire of %g kW on a square burner 0.3 m across, in open air: the flame, its puffing, and the plume above
    it. Low-Mach reacting flow in the manner of NIST's Fire Dynamics Simulator; firetest compares the averaged flame
    height with Heskestad's correlation and the plume with McCaffrey's."""
    write('methane_fire', {
        'domain': 'fire', 'title': 'A 100 kW methane fire and its plume',
        'fuel': METHANE,
        'burner': {'box_m': [-0.15, -0.15, 0.15, 0.15], 'heat_release_rate_w': Q, 'ramp_s': 1.0,
                   'source': 'a porous square burner 0.3 m across, as in McCaffrey\'s experiments (1979)'},
        'box_m': [-0.8, -0.8, 0.0, 0.8, 0.8, 3.2], 'cell_m': dx, 'ambient_temperature_k': 293.15,
        'run': {'end_s': 12.0, 'frames': 96, 'average_from_s': 6.0},
    })


def office_hvac_3d(dx=0.1):
    """An office of 4.8 by 3.2 by 2.4 m cooled by a ceiling diffuser: air at 17 C falls from the supply, spreads over
    the floor and rises again in the plumes of two seated people and two computers, and leaves through a return grille
    in the ceiling. The room's air in 3D: walls, desks as obstacles, the supply as a vent, the return as an opening,
    the people and machines as heat sources (fire.h's room engine; roomtest verifies it)."""
    write('office_hvac_3d', {
        'domain': 'fire', 'title': 'An office cooled by a ceiling diffuser',
        'box_m': [0, 0, 0, 4.8, 3.2, 2.4], 'cell_m': dx, 'ambient_temperature_k': 294.15,
        'reference_temperature_k': 294.15,
        'sides': {k: 'wall' for k in ('x-', 'x+', 'y-', 'y+', 'z-', 'z+')},
        'turbulence': 'vreman',
        'vents': [{'name': 'supply diffuser', 'side': 'z+', 'rect_m': [1.0, 1.4, 1.4, 1.8], 'speed_m_s': 0.5,
                   'temperature_k': 290.15}],
        'openings': [{'name': 'return grille', 'side': 'z+', 'rect_m': [3.6, 1.4, 4.0, 1.8]}],
        'obstacles': [{'name': 'desk 1', 'box_m': [0.6, 0.4, 0.7, 2.0, 1.2, 0.8]},
                      {'name': 'desk 2', 'box_m': [2.8, 2.0, 0.7, 4.2, 2.8, 0.8]}],
        'heat_sources': [
            {'name': 'person 1', 'box_m': [1.1, 1.3, 0.0, 1.5, 1.6, 1.2], 'power_w': 75},
            {'name': 'person 2', 'box_m': [3.3, 1.6, 0.0, 3.7, 1.9, 1.2], 'power_w': 75},
            {'name': 'computer 1', 'box_m': [1.8, 0.5, 0.8, 2.0, 0.7, 1.2], 'power_w': 100},
            {'name': 'computer 2', 'box_m': [2.9, 2.5, 0.8, 3.1, 2.7, 1.2], 'power_w': 100}],
        'source': 'demonstration: the room, its furniture and its loads are chosen, not from a building. A seated '
                  'person gives about 75 W of sensible heat (ASHRAE Handbook, Fundamentals, 2021, ch. 18); the supply, '
                  '0.08 m^3/s at 17 C, is about eight air changes an hour.',
        'run': {'end_s': 600.0, 'frames': 120, 'average_from_s': 300.0},
    })


def data_centre_3d(dx=0.1):
    """A data hall with a cold aisle between two rows of racks, in 3D: cold air rises through perforated floor
    tiles, the racks' fans pull it through the servers, which heat it, into the hot aisles, and the ceiling returns
    take it back. The tiles supply a fifth less air than the racks draw, so hot air curls over the tops and around the
    ends of the rows into the cold aisle: the recirculation that makes hot spots in real halls."""
    rows = []
    for y0, y1, v in ((0.8, 1.8, -0.6), (3.0, 4.0, 0.6)):
        rows.append((y0, y1, v))
    fans, heat, obst = [], [], []
    for i, (y0, y1, v) in enumerate(rows):
        fans.append({'name': 'rack row %d' % (i + 1), 'box_m': [1.6, y0, 0.0, 4.8, y1, 2.0], 'axis': 'y', 'speed_m_s': v})
        heat.append({'name': 'servers, row %d' % (i + 1), 'box_m': [1.6, y0, 0.0, 4.8, y1, 2.0], 'power_w': 24000})
        obst.append({'name': 'row %d roof' % (i + 1), 'box_m': [1.6, y0, 2.0, 4.8, y1, 2.1]})
        obst.append({'name': 'row %d end' % (i + 1), 'box_m': [1.5, y0, 0.0, 1.6, y1, 2.1]})
        obst.append({'name': 'row %d end' % (i + 1), 'box_m': [4.8, y0, 0.0, 4.9, y1, 2.1]})
    write('data_centre_3d', {
        'domain': 'fire', 'title': 'A data hall: cold aisle, hot aisles and recirculation',
        'box_m': [0, 0, 0, 6.4, 4.8, 3.2], 'cell_m': dx, 'ambient_temperature_k': 297.15,
        'reference_temperature_k': 297.15,
        'sides': {k: 'wall' for k in ('x-', 'x+', 'y-', 'y+', 'z-', 'z+')},
        'turbulence': 'vreman',
        'vents': [{'name': 'perforated tiles', 'side': 'z-', 'rect_m': [1.6, 1.8, 4.8, 3.0], 'speed_m_s': 1.6,
                   'temperature_k': 291.15}],
        'openings': [{'name': 'return, row 1', 'side': 'z+', 'rect_m': [1.6, 0.0, 4.8, 0.8]},
                     {'name': 'return, row 2', 'side': 'z+', 'rect_m': [1.6, 4.0, 4.8, 4.8]}],
        'fans': fans, 'heat_sources': heat, 'obstacles': obst,
        'source': 'demonstration: the hall is chosen, not measured. Each row of four racks dissipates 24 kW and draws '
                  '3.8 m^3/s through its fronts (0.6 m/s over 6.4 m^2); the tiles supply 6.1 m^3/s at 18 C.',
        'run': {'end_s': 120.0, 'frames': 120, 'average_from_s': 60.0},
    })


def room_fire_3d(Q=250e3, dx=0.1):
    """A 250 kW methane fire in a room 3.2 m square and 2.4 m high with one doorway, and the outside beyond it: the
    plume hits the ceiling, the hot layer spreads and deepens, and smoke pours out of the top of the door while fresh
    air comes in under it (the classic compartment fire of Steckler, Quintiere and Rinkinen, 1982, larger here). Walls
    and ceiling are obstacles; the fire solver's room engine."""
    walls = [
        {'name': 'front wall', 'box_m': [3.2, 0.0, 0.0, 3.4, 1.2, 2.6]},
        {'name': 'front wall', 'box_m': [3.2, 2.0, 0.0, 3.4, 3.2, 2.6]},
        {'name': 'lintel', 'box_m': [3.2, 1.2, 2.0, 3.4, 2.0, 2.6]},
        {'name': 'ceiling', 'box_m': [0.0, 0.0, 2.4, 3.4, 3.2, 2.6]},
    ]
    write('room_fire_3d', {
        'domain': 'fire', 'title': 'A fire in a room: the hot layer and the doorway',
        'fuel': METHANE,
        'burner': {'box_m': [1.35, 1.35, 1.85, 1.85], 'heat_release_rate_w': Q, 'ramp_s': 5.0,
                   'source': 'a square gas burner 0.5 m across (demonstration)'},
        'box_m': [0, 0, 0, 4.8, 3.2, 3.2], 'cell_m': dx, 'ambient_temperature_k': 293.15,
        'sides': {'x-': 'wall', 'y-': 'wall', 'y+': 'wall', 'z-': 'wall', 'x+': 'open', 'z+': 'open'},
        'obstacles': walls,
        'source': 'demonstration: the room and the fire are chosen; 10 cm cells give D* / dx = 5.4, coarse by FDS\'s '
                  'guidance for a fire (4 to 16), fine enough for the layer and the door flows, not for the flame.',
        'run': {'end_s': 60.0, 'frames': 120, 'average_from_s': 30.0},
    })


def jelly_roll_lgm50():
    """The LG M50's wound layers, from the Chen2020 parameter set (Chen et al., J. Electrochem. Soc. 167 (2020)
    080534, as tabulated by PyBaMM): one repeat of copper foil, two negative coatings, two separators, two positive
    coatings and aluminium foil. Across the layers (radially) they conduct in series, along them (axially) in parallel;
    the heat capacity is their volume-weighted sum. The electrolyte's own heat capacity is not in the set."""
    layers = [  # thickness m, conductivity W/(m K), density kg/m^3, specific heat J/(kg K)
        (12e-6, 401.0, 8960.0, 385.0), (2 * 85.2e-6, 1.7, 1657.0, 700.0), (2 * 12e-6, 0.16, 397.0, 700.0),
        (2 * 75.6e-6, 2.1, 3262.0, 700.0), (16e-6, 237.0, 2700.0, 897.0)]
    L = sum(l[0] for l in layers)
    kr = L / sum(l[0] / l[1] for l in layers)
    kz = sum(l[0] * l[1] for l in layers) / L
    rc = sum(l[0] * l[2] * l[3] for l in layers) / L
    return round(kr, 4), round(kz, 3), round(rc, -3)


def battery_module(current=120.0, dx=0.0015):
    """A module of twelve LG M50 cells (21700) in parallel, standing on a liquid-cooled aluminium plate, discharged at
    2C (%g A): each cell computed by the Doyle-Fuller-Newman model at its own temperature, the heat conducted through
    the module in 3D, the coolant warming as it flows under the plate. One cell's interconnect is four times as
    resistive as the others', as a poor weld would make it; it delivers less early and more late, and the cells
    downstream in the coolant run warmer."""
    kr, kz, rc = jelly_roll_lgm50()
    write('battery_module', {
        'domain': 'battery', 'title': 'Twelve cells in parallel on a cold plate, 2C',
        'cell': {'parameters': 'lgm50_chen2020', 'volumes_per_region': 8, 'shells': 8,
                 'source': 'LG M50 (21700, NMC 811 and graphite-SiOx): Chen et al., J. Electrochem. Soc. 167 (2020) '
                           '080534, the Chen2020 parameter set as tabulated by PyBaMM (BSD-3-Clause)'},
        'module': {'rows': 3, 'cols': 4, 'pitch_m': 0.023, 'radius_m': 0.0105, 'height_m': 0.070, 'plate_m': 0.003,
                   'margin_m': 0.003, 'grid_m': dx, 'layers': 14},
        'thermal': {
            'jelly_roll': {'radial_w_mk': kr, 'axial_w_mk': kz, 'heat_capacity_j_m3k': rc,
                           'source': 'computed from the Chen2020 set\'s layer thicknesses, conductivities, densities and '
                                     'heat capacities (tools/lab_scenarios.py jelly_roll_lgm50): layers in series across, '
                                     'in parallel along; the electrolyte\'s heat capacity is not in the set'},
            'plate': {'conductivity_w_mk': 237.0, 'heat_capacity_j_m3k': 2700.0 * 897.0,
                      'source': 'aluminium, the values of the Chen2020 set\'s positive current collector'},
            'gap': {'conductivity_w_mk': 0.0263, 'heat_capacity_j_m3k': 1.1614 * 1007.0,
                    'source': 'air at 300 K, Incropera, DeWitt, Bergman and Lavine, Fundamentals of Heat and Mass '
                              'Transfer, 6th ed. (2007), table A.4'}},
        'cooling': {'coolant_inlet_k': 298.15, 'coolant_capacity_rate_w_k': 18.0, 'plate_coefficient_w_m2k': 1000.0,
                    'air_coefficient_w_m2k': 5.0, 'air_k': 298.15,
                    'source': 'demonstration: 5 g/s of water-glycol (18 W/K) under the plate, 1000 W/(m^2 K) into it, '
                              'still air around'},
        'electrical': {'interconnect_ohm': 1e-3, 'weak_cells': [{'cell': 5, 'interconnect_ohm': 4e-3}],
                       'source': 'demonstration: 1 milliohm per cell for its weld and strip, one poor weld at 4'},
        'load': {'current_a': current, 'cutoff_v': 2.5},
        'initial_temperature_k': 298.15,
        'run': {'dt_s': 10.0, 'end_s': 2000.0, 'frames': 60},
    })


def melt_pool_flow():
    """A laser melts one track into 316L with the flow in the melt computed: the surface tension falls as the metal
    heats (dsigma/dT < 0), so the surface is pulled from the hot centre under the beam to the cooler rim at metres per
    second, carrying heat outward: the pool grows wider and shallower than conduction alone makes it (melttest M7).
    Incompressible flow in the liquid, the solid held by a Carman-Kozeny drag, the top flat (no keyhole)."""
    write('melt_pool_flow', {
        'domain': 'melt', 'title': 'Marangoni flow in a laser melt pool',
        'metal': SS316L_MELT,
        'melt_flow': {'viscosity_pa_s': 6e-3, 'dsigma_dT_n_mk': -4e-4,
                      'source': 'demonstration values of the magnitude reported for liquid iron-based alloys: viscosity '
                                '6 mPa s, surface tension falling 0.4 mN/m per kelvin (a steel low in sulphur; with more '
                                'sulphur dsigma/dT turns positive and the flow reverses)'},
        'block': {'size_m': [0.8e-3, 0.3e-3, 0.15e-3], 'origin_m': [0.0, 0.0, -0.15e-3], 'cell_m': 5e-6, 'output_every': 2},
        'beam': {'power_w': 200.0, 'absorptivity': 0.35, 'radius_m': 40e-6,
                 'tracks': [{'from_m': [0.1e-3, 0.15e-3], 'to_m': [0.7e-3, 0.15e-3], 'speed_m_s': 0.8}],
                 'source': 'as laser_tracks: 200 W, 0.8 m/s, 80 um spot; absorptivity 0.35 (demonstration)'},
        'initial_temperature_k': 293.15, 'convection_w_m2k': 10.0,
        'run': {'end_s': 0.9e-3, 'frames': 45},
    })


RS180 = {'re_ohm': 6.4, 'le_h': 0.73e-3, 'bl_t_m': 7.82, 'mms_kg': 17.9e-3, 'cms_m_n': 1.12e-3, 'sd_m2': 124.7e-4,
         'fs_hz': 35.7, 'qms': 1.22,
         'source': 'Dayton Audio RS180-8 specification sheet (part 295-355): Thiele-Small parameters as published'}


def loudspeaker():
    """A 7 inch woofer (the RS180-8 of actest A6 and A7) in the wall of a small room, driven by a 400 Hz burst at
    2.83 V RMS: the coil's current moves the cone, the cone pushes the air, the sound fills the room. Wall absorption
    is a demonstration value, not a measured room."""
    write('loudspeaker', {
        'domain': 'acoustic', 'title': 'A loudspeaker fills a room: coil, cone and sound',
        'provenance': {'source': 'driver from its maker\'s sheet; room size and wall absorption 0.15 are demonstration values'},
        'room_m': [3.0, 2.4, 2.4], 'dx_m': 0.03, 'speed_of_sound_m_s': 343.2, 'density_kg_m3': 1.204,
        'materials': [{'name': 'plaster', 'absorption_normal': 0.15}, {'name': 'sofa', 'absorption_normal': 0.7}],
        'walls': {'all': 'plaster'},
        'boxes': [{'box_m': [2.1, 0.5, 0.0, 2.9, 1.9, 0.45], 'material': 'sofa'}],
        'speaker': {'face': 'x_low', 'centre_m': [1.2, 1.1], 'driver': RS180,
                    'drive': {'voltage_peak_v': round(2.83 * 2 ** 0.5, 3), 'frequency_hz': 400, 'cycles': 4}},
        'receivers': [{'position_m': [1.0, 1.2, 1.1]}, {'position_m': [2.4, 1.2, 1.1]}],
        'run': {'end_time_s': 0.016, 'frames': 48, 'volume': True},
    })


AIR = {'kinematic_viscosity_m2_s': 15.89e-6, 'density_kg_m3': 1.1614,
       'source': INCROPERA % 'table A.4 (air at 300 K and 1 atm): nu 15.89e-6 m2/s, rho 1.1614 kg/m3'}


def sphere_wake_3d():
    """A sphere at Reynolds number 300 sheds a chain of hairpin vortices (the wake is periodic and three-dimensional
    here; Johnson and Patel 1999 measure and compute Strouhal 0.137, drag 0.656)."""
    D, Re = 0.02, 300
    U = Re * AIR['kinematic_viscosity_m2_s'] / D
    write('sphere_wake_3d', {
        'domain': 'flow', 'model': 'lbm3d', 'title': 'A sphere sheds hairpin vortices: Reynolds number 300, in 3D',
        'fluid': AIR, 'stream': {'speed_m_s': round(U, 6)},
        'box_m': [-3 * D, -3 * D, -3 * D, 9 * D, 3 * D, 3 * D], 'cell_m': D / 16, 'lattice_speed': 0.05, 'sides': 'slip', 'sponge_cells': 8,
        'bodies': [{'shape': 'sphere', 'centre_m': [0, 0, 0], 'radius_m': D / 2}],
        'disturbance': {'noise_fraction': 0.05, 'inlet_noise_fraction': 0.01,
                        'note': 'random disturbances in the start and at the inlet, as in any real stream; seeded, so the run repeats'},
        'reference': {'area_m2': 3.141592653589793 * D * D / 4, 'length_m': D},
        'run': {'end_s': round(80 * D / U, 4), 'frames': 48, 'output_stride': 2, 'output_from_s': round(60 * D / U, 4),
                'average_from_s': round(50 * D / U, 4)},
    })


def cylinder_mounted_3d():
    """A finite cylinder standing on the ground in a stream (a chimney, a bridge pier), Reynolds number 1000: a
    horseshoe vortex wraps its foot, its free top sheds tip vortices, and the wake between them breaks into arches and
    tangled vortex loops. Three-dimensional by nature: no periodic span. The stream carries small random disturbances
    (5 % in the initial field, 2 % at the inlet), as every real stream does. Large-eddy simulation (Smagorinsky 0.1)."""
    D, Re, H = 0.02, 1000, 4
    U = Re * AIR['kinematic_viscosity_m2_s'] / D
    write('cylinder_mounted_3d', {
        'domain': 'flow', 'model': 'lbm3d', 'title': 'A cylinder standing in a stream: horseshoe, tip and wake vortices, Re 1000',
        'fluid': AIR, 'stream': {'speed_m_s': round(U, 6)},
        'box_m': [-4 * D, -5 * D, 0, 16 * D, 5 * D, 8 * D], 'cell_m': D / 12, 'lattice_speed': 0.05, 'sides_y': 'periodic', 'sides_z': 'slip',
        'les': {'smagorinsky': 0.1}, 'collision': 'regularized', 'sponge_cells': 8, 'floor': 'wall',
        'bodies': [{'shape': 'cylinder', 'centre_m': [0, 0, 0.5 * H * D - 0.5 * D], 'radius_m': D / 2, 'axis': 'z', 'length_m': H * D + D}],
        'disturbance': {'noise_fraction': 0.05, 'inlet_noise_fraction': 0.02,
                        'note': 'random disturbances in the start and at the inlet, as in any real stream; seeded, so the run repeats'},
        'reference': {'area_m2': D * H * D, 'length_m': D},
        'run': {'end_s': round(60 * D / U, 4), 'frames': 60, 'output_stride': 2, 'output_from_s': round(30 * D / U, 4),
                'average_from_s': round(25 * D / U, 4)},
    })


def wing_3d(aoa=10.0):
    """A finite wing, NACA 0012, aspect ratio 3, at %g degrees and Reynolds number 100 000: its tip vortices roll up.
    A large-eddy simulation on a coarse grid (24 cells to the chord): the forces are an estimate, not a validation."""
    c, b, Re = 0.1, 0.3, 1e5
    U = Re * AIR['kinematic_viscosity_m2_s'] / c
    write('wing_3d_%d' % int(aoa), {
        'domain': 'flow', 'model': 'lbm3d', 'title': 'A finite wing at %g degrees: tip vortices, NACA 0012, Re 100 000' % aoa,
        'fluid': AIR, 'stream': {'speed_m_s': round(U, 4)},
        'box_m': [-1.0 * c, -1.0 * c, -1.5 * c, 4.0 * c, b + 1.0 * c, 1.5 * c], 'cell_m': c / 24, 'lattice_speed': 0.05, 'sides': 'slip',
        'les': {'smagorinsky': 0.12}, 'collision': 'regularized', 'sponge_cells': 8,
        'bodies': [{'shape': 'wing', 'naca': '0012', 'root_leading_edge_m': [0, 0, 0], 'chord_m': c, 'span_m': b, 'angle_deg': aoa}],
        'reference': {'area_m2': c * b, 'length_m': c},
        'run': {'end_s': round(16 * c / U, 5), 'frames': 48, 'output_stride': 1, 'output_from_s': round(6 * c / U, 5),
                'average_from_s': round(8 * c / U, 5)},
    })


def flag_3d(U=6.0):
    """A flag on a pole in a wind of %g m/s: a sheet of ripstop nylon, 0.6 m by 0.4 m, held along the pole, in air.
    The flag and the wind are solved together (immersed boundary, fluid-structure interaction): it flutters and sheds
    vortices. A large-eddy simulation on a coarse grid (40 cells along the flag): the flapping is an estimate."""
    L, Hf, dx = 0.6, 0.4, 0.015
    write('flag_3d', {
        'domain': 'flow', 'model': 'lbm3d', 'title': 'A flag flutters in a %g m/s wind' % U,
        'fluid': AIR, 'stream': {'speed_m_s': U},
        'box_m': [-0.6, -0.6, -0.45, 2.4, 0.6, 0.75], 'cell_m': dx, 'lattice_speed': 0.05, 'sides': 'slip',
        'les': {'smagorinsky': 0.15}, 'collision': 'regularized', 'sponge_cells': 10,
        'disturbance': {'noise_fraction': 0.02, 'inlet_noise_fraction': 0.005},
        'bodies': [{'shape': 'cylinder', 'centre_m': [0.0, 0.0, 0.15], 'radius_m': 0.02, 'length_m': 1.4, 'axis': 'z'}],
        'sheet': {'material': {'shear_modulus_pa': 2.0e6, 'density_kg_m3': 600.0,
                               'source': 'flag cloth of about 60 g/m2 at 0.1 mm, as ripstop nylon; stretch stiffness E h = 600 N/m, the '
                                         'soft small-strain range of a woven cloth (crimp), which keeps it within 0.2 % of its length '
                                         'under this wind; bending as a plate of that modulus (demonstration values, not measured on a '
                                         'cloth; taken as isotropic). A stiffer cloth needs a finer flow step: the flow must follow '
                                         'its stretch waves (docs/lab/fsi.md)'},
                  'thickness_m': 1e-4, 'cell_m': 0.02, 'size_m': [L, Hf], 'corner_m': [0.025, 0.0, 0.0],
                  'along': [1, 0, 0], 'across': [0, 0, 1], 'pinned': 'first_edge', 'look': 'fabric'},
        'reference': {'area_m2': L * Hf, 'length_m': L},
        'run': {'end_s': 3.0, 'frames': 60, 'output_stride': 2, 'output_from_s': 0.6, 'average_from_s': 1.0,
                'iso_level_q_per_s2': 30 * (U / L) ** 2},
    })


BLOOD = {'kinematic_viscosity_m2_s': 3.5e-3 / 1060.0, 'density_kg_m3': 1060.0,
         'source': 'whole blood at 37 C taken as a Newtonian fluid: viscosity 3.5 mPa s and density 1060 kg/m3, the values '
                   'usual for large arteries (Ku, Blood flow in arteries, Annu Rev Fluid Mech 29, 1997); blood thins at high '
                   'shear, which is not modelled'}


def heart_valve():
    """A three-leaflet valve, shaped like the aortic valve, in a vessel of 25 mm with three sinuses, through one
    heartbeat: the ejection pushes the leaflets open, a jet leaves them with vortices at its edge, vortices fill the
    sinuses behind the leaflets, and at the end of ejection they and a short backflow close the leaflets against each
    other. Leaflets, blood and their contact solved together. (In a straight tube, without sinuses, the leaflets stayed
    pressed to the wall after ejection: the sinuses are what lets the valve close.)"""
    R = 0.0125
    wave = [[0.0, 0.0], [0.05, 0.6], [0.10, 1.1], [0.15, 1.2], [0.20, 1.05], [0.25, 0.7], [0.30, 0.25], [0.33, 0.0],
            [0.36, -0.25], [0.40, -0.05], [0.43, 0.0], [0.8, 0.0]]
    write('heart_valve', {
        'domain': 'flow', 'model': 'lbm3d', 'title': 'A heart valve opens and closes through one heartbeat',
        'fluid': BLOOD,
        'stream': {'speed_m_s': 1.2, 'waveform': {'period_s': 0.8, 'points': wave,
                   'source': 'a demonstration waveform shaped like the flow through the aortic valve of an adult at rest '
                             '(peak about 1.2 m/s, ejection 0.33 s, heartbeat 0.8 s) with a backflow at closure of about 3.5 % '
                             'of the ejected volume (the closing regurgitation of a healthy valve is some 2 to 5 %); the '
                             'inflow is prescribed, so the aortic pressure that shuts a real valve is represented only by '
                             'that backflow. Not a measured patient curve'}},
        'box_m': [-0.02, -0.0185, -0.0185, 0.08, 0.0185, 0.0185], 'cell_m': 0.0006, 'lattice_speed': 0.02, 'sides': 'slip',
        'les': {'smagorinsky': 0.15}, 'collision': 'regularized', 'sponge_cells': 30, 'sponge_viscosity_lattice': 0.25,
        'outlet': 'pressure', 'disturbance': {'noise_fraction': 0.01},
        'bodies': [{'shape': 'aortic_root', 'centre_m': [0.0, 0.0, 0.0], 'radius_m': R, 'sinus_radius_m': 0.7 * R,
                    'sinus_offset_m': 0.75 * R, 'sinus_x_m': 0.001, 'sinus_phase_deg': 0.0,
                    'source': 'a straight vessel of 25 mm with three sinuses as spheres behind the leaflets, bulging to 1.45 '
                              'times its radius: the proportions of an adult aortic root, simplified (demonstration geometry)'}],
        'body_look': 'glass', 'bodies_cutaway': [0, 1, 0],
        'sheet': {'material': {'shear_modulus_pa': 0.5e6, 'density_kg_m3': 1100.0,
                               'source': 'valve leaflet tissue taken as an isotropic neo-Hookean sheet with a small-strain shear '
                                         'modulus of 0.5 MPa (the real tissue is layered, anisotropic and stiffens strongly with '
                                         'stretch; its low-strain range is of this order), 0.5 mm thick (demonstration values)'},
                  'thickness_m': 0.0005, 'gravity_m_s2': 0.0, 'self_contact_m': 0.0004, 'look': 'tissue',
                  'valve': {'centre_m': [0.0, 0.0, 0.0], 'radius_m': R, 'height_m': 0.009, 'belly_m': 0.004, 'gap_m': 0.001,
                            'nodes_around': 23}},
        'reference': {'area_m2': math.pi * R * R, 'length_m': 2 * R},
        'run': {'end_s': 0.6, 'frames': 60, 'output_stride': 1, 'output_from_s': 0.0, 'average_from_s': 0.0,
                'iso_level_q_per_s2': 200 * (1.2 / (2 * R)) ** 2},
    })


def capsule_3d(aoa=25.0):
    """The Apollo-shaped capsule of capsule_mach6 as a three-dimensional body, at the angle of attack it trims at in
    hypersonic flight (the heat shield tilted about 25 degrees to the stream by an offset centre of mass), Mach 6 at
    30 km: the bow shock is no longer axisymmetric, the shock layer thins on one side of the heat shield and thickens on
    the other, and the pressure difference gives lift. Perfect gas, inviscid."""
    prof, dims = capsule_profile()
    write('capsule_3d', {
        'domain': 'gas', 'model': 'compressible3d',
        'title': 'An Apollo-shaped capsule at Mach 6 and %g degrees: its lifting shock layer in 3D' % aoa,
        'provenance': {'shape': 'approximated from published Apollo command module dimensions; a demonstration, not the flown geometry',
                       'dimensions': dims, 'freestream': 'US Standard Atmosphere 1976 at 30 km: 1197 Pa, 226.5 K',
                       'physics': 'perfect gas, gamma 1.4, inviscid: real-gas and viscous effects of re-entry are not modelled'},
        'gas': {'gamma': 1.4, 'gas_constant_j_kgk': 287.05, 'source': 'dry air as a perfect gas (US Standard Atmosphere 1976 gas constant)'},
        'freestream': {'mach': 6.0, 'pressure_pa': 1197.0, 'temperature_k': 226.5},
        'box_m': [-2.0, 0.0, -4.0, 6.0, 6.0, 6.0], 'cell_m': 0.08,
        'faces': {'x_low': 'inflow', 'x_high': 'outflow', 'y_low': 'symmetry', 'y_high': 'outflow', 'z_low': 'outflow', 'z_high': 'outflow'},
        'bodies': [{'shape': 'revolved', 'profile_m': [[round(x, 6), round(y, 6)] for x, y in prof], 'origin_m': [0.0, 0.0, 0.0],
                    'pitch_deg': aoa}],
        'reference': {'area_m2': round(3.141592653589793 * dims['diameter_m'] ** 2 / 4, 5), 'nose_radius_m': dims['heat_shield_radius_m']},
        'run': {'end_s': 0.02, 'frames': 40},
    })


def radar_aircraft():
    """A radar at 300 MHz (a VHF search radar's band) sends a pulse at an aircraft 15 m long, 20 m away: the pulse
    wraps the fuselage, the wings and the fin, and the echo returns to the antenna, whose delay gives the range. The
    aircraft is a generic swept-wing shape made of simple bodies (a solid of revolution, NACA 0006 wings), perfectly
    conducting; a demonstration, not a particular aircraft."""
    x0, yc, zc = 12.0, 10.0, 5.0  # the nose
    fus = [[0, 0], [0.6, 0.38], [1.5, 0.66], [2.6, 0.82], [4.0, 0.9], [11.0, 0.9], [13.5, 0.6], [15.0, 0.35], [15.0, 0.0]]
    wing = dict(shape='wing', naca='0006', chord_m=4.5, tip_chord_m=1.4, span_m=5.8, sweep_deg=35.0, angle_deg=0.0)
    tail = dict(shape='wing', naca='0006', chord_m=2.3, tip_chord_m=0.9, span_m=3.0, sweep_deg=40.0, angle_deg=0.0)
    write('radar_aircraft', {
        'domain': 'em', 'title': 'A radar pulse finds an aircraft: 300 MHz, the echo and its range',
        'provenance': {'aircraft': 'a generic swept-wing shape from simple bodies, perfectly conducting; a demonstration',
                       'radar': 'a pulsed dipole at 300 MHz, vertical polarisation; the same antenna listens for the echo'},
        'cells': [360, 200, 100], 'dx_m': 0.1, 'pml_cells': 12,
        'bodies': [
            {'shape': 'revolved', 'profile_m': fus, 'origin_m': [x0, yc, zc], 'pitch_deg': 0.0},
            dict(wing, root_leading_edge_m=[x0 + 5.2, yc, zc], span_axis='y'),
            dict(wing, root_leading_edge_m=[x0 + 5.2, yc, zc], span_axis='-y'),
            dict(tail, root_leading_edge_m=[x0 + 12.4, yc, zc + 0.2], span_axis='y'),
            dict(tail, root_leading_edge_m=[x0 + 12.4, yc, zc + 0.2], span_axis='-y'),
            {'shape': 'wing', 'naca': '0008', 'chord_m': 3.0, 'tip_chord_m': 1.2, 'span_m': 3.2, 'sweep_deg': 45.0, 'angle_deg': 0.0,
             'root_leading_edge_m': [x0 + 11.0, yc, zc + 0.6], 'span_axis': 'z'},
        ],
        'radar': {'antenna_m': [2.5, 4.0, zc], 'frequency_hz': 3.0e8, 'bandwidth_hz': 1.5e8, 'amplitude_v_m': 1.0},
        'run': {'end_time_s': 1.9e-7, 'frames': 60, 'volume': True, 'volume_stride': 2},
    })


GLASS = {'youngs_modulus_pa': 72e9, 'density_kg_m3': 2500.0, 'fracture_toughness_pa_sqrt_m': 0.75e6,
         'source': 'soda-lime glass, typical values: Young\'s modulus 72 GPa, density 2.5 g/cm3, fracture toughness 0.75 MPa m^0.5 '
                   '(Varshneya and Mauro, Fundamentals of Inorganic Glasses, 3rd ed., 2019); Poisson\'s ratio 0.22 taken as the '
                   'bond-based model\'s 1/4'}


def bottle_profile(R=0.032, t=0.003, rn=0.0135, Hb=0.13, Hs=0.175, H=0.23, tb=0.004):
    """A bottle's wall in (height, radius), closed along the axis: the outer contour up, the inner one down."""
    outer = [[0.0, 0.0], [0.0, R], [Hb, R]]
    inner = []
    for k in range(1, 9):  # the shoulder, a cosine blend from the body to the neck
        u = k / 8
        a = Hb + (Hs - Hb) * u
        outer.append([a, rn + (R - rn) * 0.5 * (1 + math.cos(math.pi * u))])
    outer.append([H, rn])
    inner.append([H, rn - t])
    for k in range(8, 0, -1):
        u = k / 8
        a = Hb + (Hs - Hb) * u
        inner.append([a, rn - t + (R - rn) * 0.5 * (1 + math.cos(math.pi * u))])
    inner += [[Hb, R - t], [tb, R - t], [tb, 0.0]]
    return [[round(a, 6), round(r, 6)] for a, r in outer + inner]


def glass_bottle():
    """A glass bottle dropped from 1.3 m lands on the edge of its base on concrete and shatters. Brittle fracture by
    peridynamics: cracks start where the bonds are stretched past the glass's critical stretch, run and branch on
    their own. The floor is rigid (concrete is far stiffer than the loads here need)."""
    write('glass_bottle', {
        'domain': 'fracture', 'title': 'A glass bottle falls 1.3 m onto concrete and shatters',
        'material': GLASS, 'spacing_m': 0.001,
        'bodies': [{'shape': 'revolved', 'profile_m': bottle_profile(), 'origin_m': [0.0, 0.0, 0.0065], 'pitch_deg': 80.0}],
        'box_m': [-0.06, -0.04, 0.0, 0.1, 0.04, 0.25],
        'velocity_m_s': [0.0, 0.0, -5.0],
        'floor': {'z_m': 0.0, 'source': 'a rigid floor: concrete (about 30 GPa) is taken as unyielding'},
        'run': {'end_s': 0.0025, 'frames': 60},
    })


RUBBER = {'shear_modulus_pa': 0.42e6, 'density_kg_m3': 930.0, 'viscosity_pa_s': 20.0,
          'source': 'vulcanised natural rubber: shear modulus 0.42 MPa, the small-strain value of Ogden\'s (1972) fit to '
                    'Treloar\'s (1944) tests; density 930 kg/m3, a typical natural rubber; a Kelvin-Voigt viscosity of '
                    '20 Pa s, which gives a loss factor of 0.1 at about 330 Hz (natural rubber\'s is 0.1 to 0.2 at audio '
                    'frequencies): a demonstration value, not fitted to a test'}


def rubber_sheet():
    """A square sheet of natural rubber, 0.5 mm thick, falls from 20 cm onto a ball resting on a steel floor, drapes
    over it and settles into folds. Stretch, bending, contact and friction; the sheet does not touch itself."""
    write('rubber_sheet', {
        'domain': 'sheet', 'title': 'A rubber sheet falls onto a ball and drapes',
        'material': RUBBER,
        'sheet': {'size_m': [0.8, 0.8], 'centre_m': [0.0, 0.0, 0.5], 'thickness_m': 0.0005, 'cell_m': 0.005,
                  'contact_distance_m': 0.002},
        'bodies': [{'shape': 'sphere', 'centre_m': [0.0, 0.0, 0.15], 'radius_m': 0.15}],
        'floor': {'z_m': 0.0, 'source': 'a rigid steel floor'},
        'friction': 0.8, 'damping_1_s': 1.0,
        'note': 'Friction 0.8 is a typical dry value for rubber on a smooth hard surface; the damping of 1/s stands for '
                'the air and the rubber\'s own losses, not measured (demonstration values).',
        'body_look': 'aluminium',
        'run': {'end_s': 1.2, 'frames': 72},
    })


PICHLER = ('Pichler, Simonds, Sowards and Pottlacher, Measurements of thermophysical properties of solid and liquid NIST '
           'SRM 316L stainless steel, J Mater Sci 55 (2020) 4081')
SS316L_MELT = {
    'density_kg_m3': 7904.0, 'solidus_k': 1675.0, 'liquidus_k': 1708.0, 'latent_heat_j_kg': 2.9e5,
    'specific_heat_solid_j_kgk': 580.0, 'conductivity_solid_w_mk': 22.0,
    'specific_heat_liquid_j_kgk': 800.0, 'conductivity_liquid_w_mk': 30.0, 'emissivity': 0.4,
    'source': 'wrought 316L (the materials library, src/ctl/materials.json, record ss316l_lpbf): density, solidus 1675 K, '
              'liquidus 1708 K and latent heat 290 kJ/kg from ' + PICHLER + '; solid specific heat 580 J/(kg K), the mean of '
              'their DSC values from 200 to 980 C, and conductivity 22 W/(m K), the Georgia Tech measurements near 700 C, '
              'held constant; the liquid\'s 800 J/(kg K) and 30 W/(m K) and the emissivity 0.4 are assumed, of the '
              'magnitude reported for liquid stainless steels (demonstration values)',
}


def laser_tracks():
    """A laser melts three tracks side by side into a 316L plate, as in laser powder bed fusion (without the powder):
    200 W at 0.8 m/s, 80 um spot, 80 um between tracks, each track turning back on the last. The melt pool runs with
    the beam, the metal solidifies behind it, and each track remelts the edge of the one before."""
    y = [0.22e-3, 0.30e-3, 0.38e-3]
    tracks = []
    for i, yy in enumerate(y):
        a, b = (0.2e-3, 1.8e-3) if i % 2 == 0 else (1.8e-3, 0.2e-3)
        tracks.append({'from_m': [a, yy], 'to_m': [b, yy], 'speed_m_s': 0.8, 'pause_s': 0.0 if i == 0 else 5e-5})
    write('laser_tracks', {
        'domain': 'melt', 'title': 'A laser melts three tracks into steel',
        'metal': SS316L_MELT,
        'block': {'size_m': [2.0e-3, 0.6e-3, 0.3e-3], 'origin_m': [0.0, 0.0, -0.3e-3], 'cell_m': 5e-6, 'output_every': 2,
                  'bottom_held_k': 293.15},
        'beam': {'power_w': 200.0, 'absorptivity': 0.35, 'radius_m': 40e-6, 'tracks': tracks,
                 'source': 'a typical LPBF process point (200 W, 0.8 m/s, 80 um spot); absorptivity 0.35 of a bare steel '
                           'surface at 1.07 um, a demonstration value (a powder bed absorbs more)'},
        'initial_temperature_k': 293.15, 'convection_w_m2k': 10.0,
        'run': {'end_s': 6.8e-3, 'frames': 68},
    })


def induction_gear(current=10000.0, f=30e3):
    """Induction hardening of a gear: a 24-tooth steel gear inside a water-cooled ring coil at %g kHz. The eddy
    currents crowd into the gear's surface (skin depth about 3 mm) and most into the teeth, which heat first; the heat
    then soaks inwards. One tooth is computed, the gear drawn whole by its symmetry."""
    write('induction_gear', {
        'domain': 'magnet', 'title': 'Induction hardening of a gear: eddy currents heat the teeth first',
        'induction': {
            'gear': {'teeth': 24, 'bore_radius_m': 0.015, 'root_radius_m': 0.040, 'tip_radius_m': 0.046, 'thickness_m': 0.020,
                     'tooth_fraction_at_root': 0.55, 'tooth_fraction_at_tip': 0.35,
                     'source': 'a demonstration gear, module about 3.8 mm; not a particular part'},
            'coil': {'inner_radius_m': 0.050, 'outer_radius_m': 0.056, 'height_m': 0.018, 'current_a': current, 'frequency_hz': f,
                     'source': 'a single-turn water-cooled copper coil; current amplitude and frequency in the range of gear-hardening machines'},
            'steel': {'conductivity_s_m': 0.86e6, 'relative_permeability': 1.0, 'thermal_conductivity_w_mk': 26.0,
                      'volumetric_heat_capacity_j_m3k': 7700 * 650.0,
                      'source': 'AISI 1045 carbon steel at 800 C, above its Curie point (so non-magnetic): resistivity 1.16 micro-ohm m, '
                                'thermal conductivity 26 W/(m K), specific heat 650 J/(kg K), density 7700 kg/m3 (Rudnev, Loveless and Cook, '
                                'Handbook of Induction Heating, 2nd ed., 2017); held constant through the run'},
            'grid': {'sectors_per_tooth': 24, 'surface_cell_m': 0.0005, 'outer_radius_m': 0.080, 'axial_extent_m': 0.060},
            'heat': {'duration_s': 3.0, 'frames': 60, 'steps_per_frame': 5, 'ambient_k': 293.15},
        },
    })


def sloshing_tank():
    """The first sloshing mode of a tank, the verification case W2 in 3D: 0.5 m of water in a tank 1 m long."""
    write('sloshing_tank', {
        'domain': 'water',
        'title': 'Water sloshes in a tank',
        'note': 'The free surface starts as 0.05 m cos(pi x / L) and swings at the first mode; linear theory gives a period '
                'of 1.18 s (wtest W2 checks it to 0.21 % on a slab).',
        'particle_spacing_m': 0.02,
        'gravity_m_s2': 9.80665,
        'water': {'density_kg_m3': WATER['density_kg_m3'], 'source': WATER['source']},
        'tank': {'size_m': [1.0, 0.4, 0.8]},
        'sloshing': {'depth_m': 0.5, 'amplitude_m': 0.05},
        'run': {'end_s': 3.0, 'frames': 91},
    })


if __name__ == '__main__':
    cylinder_photo(0.5, 'cylinder_re0p5', 30, rake=True, u=0.01, D=24, width=16)
    cylinder_photo(26, 'cylinder_re26', 60)
    cylinder_photo(105, 'cylinder_re105', 160, shoulders=True)
    for a in (0, 5, 10, 15, 20):
        aerofoil_photo(a)
    taylor()
    sphere_plate()
    whipple_shield()
    crater()
    chip_cold_plate()
    # data_centre_aisle() and hvac_room(), the 2D rooms, left the library on 2026-09-26: office_hvac_3d and
    # data_centre_3d replace them in 3D (docs/lab/rooms.md)
    micro_mixer()
    heat_exchanger()
    seat_climate()
    crossflow_fan()
    pm_motor()
    pm_motor('pm_motor_cogging', J=0.0)
    pm_motor('pm_motor_drive', drive=True)
    pm_motor('pm_motor_3d', three_d={'source': 'demonstration: stack length and end space chosen, not from a product',
                                     'stack_length_m': 0.05, 'end_space_m': 0.03, 'end_turn_m': 0.012,
                                     'outer_radius_m': 0.065, 'sectors': 144, 'stack_layers': 10, 'radial_cell_m': 0.002})
    pm_motor('pm_motor_3d_drive', drive3d=True,
             three_d={'source': 'demonstration: stack length and end space chosen, not from a product',
                      'stack_length_m': 0.05, 'end_space_m': 0.03, 'end_turn_m': 0.012,
                      'outer_radius_m': 0.065, 'sectors': 144, 'stack_layers': 10, 'radial_cell_m': 0.002})
    shoebox_hall()
    capsule(4)
    capsule(2)
    double_mach()
    shock_helium()
    nozzle()
    black_hole()
    dam_break()
    sloshing_tank()
    loudspeaker()
    capsule_3d()
    radar_aircraft()
    glass_bottle()
    induction_gear()
    rubber_sheet()
    flag_3d()
    heart_valve()
    breaking_wave()
    methane_fire()
    office_hvac_3d()
    data_centre_3d()
    room_fire_3d()
    battery_module()
    melt_pool_flow()
    laser_tracks()
    sphere_wake_3d()
    cylinder_mounted_3d()
    wing_3d(10.0)
