# Mechanisms: gears and elastic parts in contact (`src/lab/mechanism`)

Up: [the lab](README.md) · code: `src/lab/mechanism/` · test: `tools/geartest.c` · films: [gear-pair-mesh.gif](../media/lab/gear-pair-mesh.gif), [gear-pair-closeup.gif](../media/lab/gear-pair-closeup.gif), [planetary-stage.gif](../media/lab/planetary-stage.gif), [planetary-stage-closeup.gif](../media/lab/planetary-stage-closeup.gif), [gear-tooth-bending.gif](../media/lab/gear-tooth-bending.gif), [robot-gearbox-two-stage.gif](../media/lab/robot-gearbox-two-stage.gif), [robot-wolfrom-stage.gif](../media/lab/robot-wolfrom-stage.gif), [helical-gearbox-three-shaft.gif](../media/lab/helical-gearbox-three-shaft.gif), [planet-load-sharing.gif](../media/lab/planet-load-sharing.gif)

## What it is

Finite elements for assemblies of parts that touch and transmit load, starting with the most common one in
machines and robot joints: a pair of spur gears in mesh, and the single-tooth bending test that rates them.

- **Gears as a cutter makes them** (`gear.h`): the tooth space is the envelope of a generating rack (ISO 53 / ISO 21771
  basic rack with its tip fillet, any profile shift, addendum, dedendum and tip radius) rolling on the pitch circle, so
  the flank is the exact involute and the root is the exact trochoid the cutter leaves; flank thinning for backlash and
  linear tip relief on top. A fillet drawn as an arc would put the root stress in the wrong place.
- **Structured hexahedral meshes** (`gearmesh.h`): per pitch, a tooth block between the flanks (graded toward both)
  and a rim block from the root surface to the bore with a boundary layer graded toward the fillet; a sector of teeth
  or the whole gear, extruded through the face. Column directions under the fillets are smoothed over a fixed physical
  length and the deep columns run in polar coordinates, so no element folds however fine the mesh (2,160 combinations
  of element counts checked in M2).
- **Elastic parts** (`mbody.h`): hex8 with incompatible modes (bending right on a few elements through a thickness),
  stiffness factorised once in each part's own frame by Accelerate's sparse supernodal Cholesky (`mfactor.h`), and the
  condensed compliance of the nodes where it may touch another part.
- **Contact** (`mcontact.h`, `mgearpair.h`): frictionless unilateral contact solved exactly (active-set pivoting with the
  driving torque's balance) on the parts' condensed compliances. Each tooth pair touches where their exact outlines
  come closest (searched both ways, so a tip corner on a flank is found); per layer of nodes across the face its force
  is a Hertzian strip at least an element wide, and the flattening narrower than that is added as the half-space's
  line-contact compliance (2 / (pi E*)) ln(a / b), iterated on each contact's own load. The pinion's rotation beyond
  conjugate action gives the loaded transmission error.
- **One tooth under an anvil** (`mtooth.h`): the single-tooth bending test, a normal flank load with the Hertzian
  pressure of a flat anvil, optional anvil friction, and the surface stress from the tip down the flank and fillet.
- **Internal gears** (`gear_build_internal`): a ring cut by a pinion-type shaper, the tooth space the envelope of the
  cutter's flank, corner and tip land as it rolls inside the ring, so the ring's flanks are its exact involutes and its
  fillet the exact path of the cutter's corner; meshed by inversion in the reference circle through the same mesher
  (`gear_mesh_build_internal`: inversion keeps angles, so the elements keep their shapes).
- **Assemblies of gears** (`mgearpart.h`): a gear as one part of a mechanism, with its axis anywhere and its own
  rotation as a rigid degree of freedom; contact between any two parts in mesh, external with external or with
  internal; the contact problem with several rigid degrees of freedom (`mcontact_solve_q`: each in equilibrium under
  its own torque, the closed set solved through the Schur complement of the bordered system).
- **A planetary stage** (`mplanet.h`): a sun, N planets on a carrier's pins and a ring, the reduction stage of most
  robot joints. The ring and carrier held, a torque on the sun, the stage placed at any carrier angle by its kinematics;
  each planet's phase from the ring's teeth and the sun's from planet 0, checked by every mesh's unloaded gap. Out:
  every tooth pair's force, each planet's share of the torque (unequal when a pin is out of place, or when the planets
  mesh out of phase on a fixed sun), the sun's wind-up (the stage's torsional compliance), the reactions, the stress.
  The sun may float (`sun_floating`): its sideways translation two more rigid degrees of freedom, which evens the
  shares.
- **Helical gears** (`helix_angle_deg`, positive a right-hand helix): the normal module and pressure angle as gears
  are specified and cut, the hob's normal section seen in the transverse plane as the generating rack (the transverse
  pressure angle, the tip fillet an ellipse), rings cut by a helical shaper of the ring's hand. The mesh is the
  transverse section turned by twist z = z tan(beta) / r through the face (the involute helicoid), and the contact acts
  along the helicoid's normal, which leans out of the transverse plane by the base helix angle: each layer of nodes
  finds its own tooth pairs (the contact lines run diagonally across the face), and a helical mesh pushes its gears
  apart along their axes by T tan(beta) / r.
- **Gear trains of any layout** (`mtrain.h`, scenario model `gear_train`): gears on shafts, shafts on axes fixed in the
  housing or carried round by carriers, any number of stages and meshes (40 gears, 32 shafts, 64 meshes). The ratios
  come from the meshes' ties (a train with more or less than one degree of freedom is refused); every gear is phased
  along the line of centres of its meshes, and a train that cannot be assembled (planets that cannot be spaced as
  asked) is refused with the misfit; compound planets are timed one by one, as they are assembled. Every shaft's
  rotation is a rigid degree of freedom (a carrier's carries its planets' axes; a floating shaft adds two
  translations), shafts are free, loaded, fixed (part of the housing) or locked (an output held while loaded), and
  both flanks of every mesh are candidates, so the train finds which side carries the load. The contacts are found,
  and their gaps linearised, where the train settles under its load: found again at each new pose until a solve moves
  no gear's teeth by more than a nanometre (where a contact switches at the pose, until the steps, halved, close in on
  it; the record's settle_nm keeps the last whole step). A train of high ratio turns its input degrees past its kinematic angle
  before it carries the load (about 5 degrees for the 69:1 Wolfrom stage); until 2026-10-22 the contacts were taken
  at the kinematic pose, and a tooth corner's gap, extrapolated over that turn, jammed the Wolfrom stage and took up to
  9 % off the two-stage gearbox's torque (geartest M22). Out: every shaft's wind-up and settled offset, the torque and
  the axial force its contacts put on it (the reactions of held ones), every mesh's force, the elastic energy stored,
  every gear's stress. Carriers' plates and pins and the shafts' hubs are drawn (rigid, grey); a cut-away wedge
  (`picture.cutaway_deg`) opens the rings and plates in the pictures. A shaft may sit on bearings
  (`support_n_per_m`: a planet's on its pin, two more degrees of freedom against that stiffness), and a ring may be
  held at lugs (`hold`: `lugs`, `lug_width_deg`, `phase_deg`, `radial_free` for a ring floating in its splines), whose
  compliance is then built for the teeth that come into mesh over the run's input range only (a solve outside it is
  refused). `run.torques_nm` sweeps the input's torque on one factorisation, `run.frames: false` keeps only the numbers,
  and the whole record goes beside the result as OUT.lab.train.json.
- **Cyclic compliance** (`mbody_zone_cyclic`): the teeth of a whole gear are alike, so its compliance between tooth i
  and tooth j depends only on j - i. It is computed from the loads on one tooth (one solve per node of a tooth instead
  of one per node of the gear) and stored as one block per tooth; a gear's model is shared by every gear like it (a
  stage's planets). The 72-tooth ring of the planetary stage: 6,480 contact nodes, compliance in 0.6 s.
- **Strain gauges**: bonded to fillets at Hofer's 30 degree tangent point, the computed maximum, an arc length from
  the tip or a radius; they read the strain along the outline averaged over the grid's length, and report E times it.

Scenarios: [planetary_robot_stage.json](../../examples/lab/planetary_robot_stage.json) (a 5:1 robot joint stage,
whole gears, five parts), [munro_gear_stiffness.json](../../examples/lab/munro_gear_stiffness.json) (one tooth pair of a high
contact ratio pair, several loads in one run), [nasa_gear_single_tooth.json](../../examples/lab/nasa_gear_single_tooth.json) (the NASA gear noise rig's
calibration: one loaded tooth pair, two gauges), [nasa_gear_pair_film.json](../../examples/lab/nasa_gear_pair_film.json)
(both gears whole, rolling through two mesh cycles), [lisle_tooth_root.json](../../examples/lab/lisle_tooth_root.json)
(a 50 mm module tooth under an anvil), and three gear trains: [robot_gearbox_two_stage_helical.json](../../examples/lab/robot_gearbox_two_stage_helical.json)
(two helical planetary stages in series, 25:1, 14 gears on 10 shafts), [robot_wolfrom_stage.json](../../examples/lab/robot_wolfrom_stage.json)
(a 69:1 Wolfrom stage: compound planets on a free carrier between a fixed and an output ring) and
[helical_gearbox_three_shaft.json](../../examples/lab/helical_gearbox_three_shaft.json) (two helical stages on three shafts,
the intermediate shaft's thrusts cancelled). Run with `build/labrun SCENARIO.json OUT.lab`; picture with
`build/labfilm OUT.lab --out DIR --field stress --azim 90 --elev 62 --center 0 0.0445 --zoom 3.4` (each train's scenario
names its own film command under `picture.film`).

## Verification (`tools/geartest.c`, in `make test-fast`)

Criteria written on 2026-10-08 before the first run of the file:

| Case | Against | Criterion | Result 2026-10-08 |
|---|---|---|---|
| M1 involutes and fillets of two generated gears | the involute's polar angle; ISO 6336-3's closed forms at the 30 degree point | 1e-9 rad; s_Fn 0.3 %, rho_F 3 % | 2e-16 rad; s_Fn 0.00 % and -0.01 %, rho_F +0.06 % and +0.43 % |
| M2 2,160 meshes, six gears of 12 to 100 teeth | no inverted element | Jacobian ratio at least 0.3 | all valid, worst 0.61 |
| M3 a slender cantilever, 80 x 4 x 4 hex8 | Timoshenko's beam | 1 % | -0.47 % (see the note in the file: the first case, L/h = 10 on 20 x 2 x 2, failed at -1.33 %) |
| M4 the NASA pair, one tooth, 61 N m | force T / r_b, arm r_b | 0.1 %, 0.5 % | -0.007 %, +0.007 % |
| M5 single-pair mesh stiffness at the pitch point | ISO 6336-1 c'_th C_B = 15.73 N/(mm um) | 6 % | 15.88 (+1.0 %) |
| M6 transmission error, 14 against 28 elements along a flank | itself | 2 % | -0.06 % |
| M7 the Lisle tooth: the 30 degree point; the root stress at the AGMA point, 15 kN | the paper's 142.8 mm; its bore-held FE model, 117 MPa | 0.5 mm; 3 % | 142.86 mm; 117.2 MPa (+0.2 %) |
| M8 a ring cut by a shaper (72 and 24 teeth) | its involute; pi m / 2 on the reference circle | 1e-9; 1e-6 | 4e-16 rad; 1e-6 |
| M9 27 ring meshes | no inverted element | Jacobian ratio at least 0.3 | all valid, worst 0.81 |
| M10 a planetary stage 18 / 3 x 27 / 72, 10 N m | the sun's torque; carrier T (1 + z_r / z_s), ring T z_r / z_s; equal shares | 1e-6; 0.1 %; 0.1 % | 1e-7; -0.006 %, -0.008 %; 1e-5 |
| M11 one pin 2 um ahead | a lumped spring estimate of that planet's share | 10 % | 0.512 against 0.541 (-5.4 %) |
| M12 the same with the sun floating | three planets on a floating sun are statically determinate: equal shares | 0.1 % | 1.00000 each, the sun moves 2.5 um |
| M13 a whole gear's cyclic compliance (15 teeth, both flanks) | the full compliance, entry by entry | 1e-9 of the largest entry | 8.5e-14 |
| M14 a helical gear (23 teeth, 22 deg, x = 0.25) and a helical ring (61 teeth, -18 deg) | the transverse involutes; m_t (pi / 2 + 2 x tan(alpha_n)); pi m_t / 2 | 1e-9 rad; 1e-9 m; 1e-6 | 2.8e-16 rad, 3.8e-10 m; 4.1e-16 rad, 1e-6 |
| M15 the twisted mesh | each layer the section turned by twist z; valid elements | 1e-12 m; Jacobian ratio 0.3 | 0; 0.544 |
| M16 a helical pair 21 / 43, 20 deg, 50 N m | (a) T z2 / z1; (b) T tan(beta) / r1; (c) ISO 6336-1's ratio of helical to spur mesh stiffness; (d) under half the spur pair's stiffness range; (e) the spur pair, at a helix of 1 deg | 0.5 %; 1 %; 5 %; one half; 1 % | +0.09 %; +0.09 %; **failed: 0.763 against 0.911 (-16 %), open (below)**; 0.31 against 4.59 N/(mm um); +0.14 % and +0.20 % |
| M17 two helical planetary stages 21 / 3 x 30 / 81, 12 deg, output held | (1 + z_r / z_s)^2; rigid statics; equal shares; T tan(beta) / r; a planet's thrusts cancel | 1e-12; 0.1 %; 0.1 %; 1 %; 1e-3 | exact; +0.021 %; 2e-8; -0.002 %, -0.004 %; 5e-4 |
| M18 a Wolfrom stage 21 / (30, 27) / 81 and 78, carrier free | 1 / i_W; T i_W; the free carrier; equal shares | 1e-12; 0.5 %; 1e-5 of T i_W; 0.1 % | exact; -0.063 %; 2.3e-6; 2.4e-6 |
| M19 two helical stages on three shafts | T (53 / 19)(47 / 17); T tan(beta) / r1; the intermediate shaft's thrusts cancel | 0.5 %; 1 %; 1 % | +0.006 %; +0.001 %; 5e-5 |
| M20 four planets on bearings of 1e8 N/m, a floating sun, a pin 20 um ahead | each bearing's force against its planet's contacts; bearings of 1e12 N/m against rigid pins | 1e-6; 1e-3 in share | 5.5e-9; equal (two planets unloaded in this case, so the second check is weaker than meant) |
| M21 the same with the ring on 11 radially free lugs | rigid statics; a solve outside the zones' range | 0.1 %; refused | +0.002 %; refused |
| M22 (2026-10-22) the 69:1 Wolfrom stage of robot_wolfrom_stage.json on coarse meshes, at seven positions where the kinematic-pose solve jammed or fell 24 to 81 % short | T i_W; one loaded flank per mesh | 1 %; none on both | +0.13, -0.89, +0.16, -0.03, +0.07, **+1.14 (failed)**, +0.12 %; none on both flanks |
| M23 (2026-10-22) virtual work where M22 departs most | T_in dθ_in/dθ_out - dU/dθ_out, loaded kinematics | 0.1 % of T i_W | **failed: -0.39 to -0.58 %** (-0.20 to +0.86 % at the other six) |

M22 and M23 (2026-10-22): the jam is gone (no mesh is loaded on both flanks), and six of seven positions are within
1 %; the seventh is +1.14 %, where a ring's tip corner carries 38 % of a planet's fixed-ring mesh on a lever arm 0.22 %
longer than the ring's base radius, a departure the Wolfrom's differential multiplies about 13 times (the teeth carry
no tip relief). The rigid kinematics show none there (+0.02 %), the loaded kinematics and the stored energy +1.5 to
+1.7 %: the corner's load comes with the deflection. Virtual work does not close to 0.1 % because the contact model is
geometrically linear (the forces do no work on flanks turned by the teeth's bending: computed apart, 0.32 N m of a
0.30 N m residual at another position), its near field is a nonlinear spring taken at its load, and at a tip corner the
output steps by up to 0.4 % between positions 0.01 degree apart, where a contact's strip or nearest point switches.
Both criteria stand as written and are recorded as failed.

Both trains over one full input turn (720 positions on a compute node, 2026-10-22; the criterion is in geartest.c's
header): the Wolfrom stage on M22's meshes had 342 positions beyond 1 % with the contacts taken at the kinematic pose
(down to -98.6 %, 71 with a mesh loaded on both flanks, the mean -15.1 %) and has 83 now (-1.69 to +1.98 %, none on
both flanks, the mean +0.020 %): windows of a degree or two each mesh cycle where a tip corner comes into or out of
mesh, positive and negative, so that over the turn the output's mean is the input's torque times the ratio, as a
frictionless train's virtual work requires. The two-stage helical planetary (its second stage on M22's coarse meshes,
its first on 6 / 8 / 4 / 4 elements, at which the kinematic-pose solve fell 2 to 9 % short in a sawtooth, the mean
-3.5 % over 0 to 60 degrees) stays within -0.014 and +0.078 % at every position, the mean +0.009 %. The 1 % criterion
fails for the Wolfrom stage and holds for the helical gearbox; no mesh is loaded on both flanks in either. The pose
took a median of five contact searches a position for the Wolfrom stage (at most 21) and three for the gearbox (at most
28); at 78 and 29 positions it settled on a switching contact.

M16 (c) failed on its first run and stays open. The ratio of the helical pair's mean mesh stiffness to its spur twin's
is 0.763 where ISO 6336-1's formula, c'_th(z_n) cos(beta) (0.75 eps_alpha + 0.25), gives 0.911. It does not come from the
discretisation (18 layers through the face instead of 6: 0.787; twice the elements in the section: 0.781), the element
(plain trilinear hexahedra: 0.773) or the gear bodies (held just under the roots: 0.735), and the helical contact meets
the spur one as the helix vanishes (M16 (e)). Over helix angles the model gives 0.98, 0.94, 0.87, 0.76 and 0.67 of the
spur pair's stiffness at 5, 10, 15, 20 and 25 degrees, a faster fall than ISO's empirical formula and than one
published finite element study, which has the mean stiffness following the contact lines' length (Liu, Ding, Wu, Liu,
Power Transmission Engineering, August 2015, 37 / 106 teeth with a face of 18 modules; here the face is 10 modules).
The criterion is reported, no longer asserted; which is right is a question for a measured helical mesh stiffness.

## Against measurements

| Measurement | Model | Status |
|---|---|---|
| Lisle, Shaw, Frazer (Mech. Mach. Theory 111, 2017): strain gauges at three points of a 50 mm module root, 5 to 15 kN, uncertainty 1.5 % | 117.8 MPa at A (measured 115.7, +1.8 %); peak 127.6 MPa (gauge B 118.8, +7.4 %) with no anvil friction; 112 MPa (-5.6 %) with the authors' assumed friction 0.125 | the measurement lies between the two; the anvil's friction was not measured and the clamping jig is not modelled (the authors' own models differ by 4 % between bore and jig) |
| Munro, Palmer, Morrish (Proc. IMechE C 215, 2001): the stiffness of one tooth pair of a 54/54 high contact ratio pair (module 3.738 mm, 18 deg, contact ratio 2.16), from transmission error with one pair and with two pairs loaded, which cancels the rig's bearings | the same reduction on the computed transmission error ([munro_gear_stiffness.json](../../examples/lab/munro_gear_stiffness.json), `flags/gear-tooth-stiffness/reference_entry.py`): half a base pitch from the pitch point 13.75 N/(mm um) between 191 and 520 N/mm (trial T5's hidden case, its answer sealed), 13.54 between 26 and 520 (the public practice case: measured 12.93, +5 %); at the pitch point 15.67 and 15.40 (the practice case's measured 14.25 read from a fitted parabola) | within the assumptions' reach: the paper gives no material (steel assumed), cutter tip radius (0.25 m; 0.15 to 0.34 moves the answer 2 %) or blank (solid to 3 modules below the root; twice as deep lowers it 4 %, because one pair loads its tooth's foundation more than two pairs do and the method does not separate that); how the stiffness changes between the two load ranges is not settled: 3.5 um of crowning gives 2 % in the model |
| Ligata (Ohio State dissertation, 2007): four helical planets on a floating sun, one carrier hole 35 or 70 um out of place, 200 to 1000 N m, shares from the ring's root strain | the overloaded pair's share at 600 to 1000 N m as close as the published models, depending on the needle bearings' stiffness (two published values); trial T6 posed with its answers sealed (its scores in the section below) | open at 200 and 400 N m, where every model unloads two planets the rig kept loaded (section above) |
| NASA gear noise rig, TM-107223 (1996) Fig. 4 and TM-103281 (1991): root strain of a single loaded tooth pair, 28/28 teeth, 85 per cent torque, gauges at the 30 degree point | 759 microstrain tensile at 20 deg roll (measured 971); the shape against roll angle matches, the level is 21 to 32 % low (tensile) and 27 to 39 % low (compressive) | open: the model agrees with ISO 6336-3, AGMA 908 and NASA's own modified Heywood analysis (which their 1990 report, TM-103232, found 10 to 15 % below the same gauges); the test gears' root geometry is not published beyond a 1.35 mm root radius |

## Planet load sharing against Ligata 2007 (trial T6)

Ligata's dissertation (The Ohio State University, 2007) measured how four helical planets share the load of a
truck-transmission set (sun 73, planets 26, ring 125 teeth, normal module 1.81 mm, helix 13.12 degrees, face 25 mm) on a
floating sun when the carrier hole of planet 1 is 35 or 70 um out of place, at sun torques of 200 to 1000 N m, from the
ring's tooth-root strain over 26 carrier turns. The model ([reference_entry.py](../../flags/planet-load-sharing/reference_entry.py)):
every gear elastic and whole, frictionless contact on both flanks, the sun floating, each planet on a bearing spring
about its pin, the ring held at its 11 splines and free radially there (as the rig's ring floats in its adaptor), the
mean over six positions of a mesh cycle. What the dissertation does not state was assumed and is listed in the entry:
the 22.5 deg normal pressure angle, the profile shifts' split, the addenda and rack, a 0.1 mm backlash, the bores,
steel, the splines' width, and above all the needle bearings' stiffness, for which the two published models of this
rig used 2e5 N/mm (Ligata, from the bearing maker's data) and 1.3e5 N/mm (Hu, 2017). Not blind: a research agent had
read the measurements out of the dissertation before the model was run. Scenario:
[planet_load_sharing_ligata.json](../../examples/lab/planet_load_sharing_ligata.json); film:
[planet-load-sharing.gif](../media/lab/planet-load-sharing.gif). The overloaded pair's share, (L1 + L3) / 2:

| Pin error, sun torque | Measured | This model, 2e5 N/mm (the reference entry) | This model, 1.3e5 N/mm | Ligata's own model (read from his plot) | Hu 2017 |
|---|---|---|---|---|---|
| +70 um, 1000 N m | sealed | 0.339 | 0.314 | 0.304 | 0.322 |
| +70 um, 800 N m | sealed | 0.362 | 0.330 | 0.319 | 0.339 |
| +70 um, 600 N m | sealed | 0.398 | 0.356 | 0.339 | 0.369 |
| +70 um, 400 N m | sealed | 0.471 | 0.408 | 0.39 | |
| +70 um, 200 N m (not a case of the trial) | 0.400 | 0.500 | 0.500 | 0.500 | |
| +35 um, 1000 N m | sealed | 0.295 | 0.282 | 0.277 | 0.286 |
| +35 um, 800 N m | sealed | 0.306 | 0.290 | 0.286 | 0.295 |
| +35 um, 600 N m | sealed | 0.324 | 0.303 | 0.296 | 0.310 |
| +35 um, 400 N m | sealed | 0.361 | 0.329 | 0.322 | |
| -70 um (planet 1 lagging), 1000 N m: (L2 + L4) / 2, the public practice case | 0.331 | 0.339 | | | |

Scored as the trial scores (the absolute error over 0.01 in share, 0.02 at 400 N m) and published, like every board,
in bands of five: 35 with the bearing maker's stiffness, 75 with Hu's, 50 for Ligata's own model; 90 clears the trial.
What the comparison shows:

- At 600 to 1000 N m the model is as close as the published models, and which of the two bearing stiffnesses fits
  better is not the same for the 70 um and the 35 um error.
- The measured excess over the equal share grows less than in proportion to the error; a linear elastic model with
  frictionless contact cannot do that. The carrier's actual hole positions were not measured, and needle bearings
  stiffen as they load.
- At 200 and 400 N m every model, this one included, has the other two planets losing their load (0.500 and 0.000 at
  200 N m for 70 um); the rig kept them loaded (0.40 and 0.10). What holds them is not known.
- The splines are worth 0.009 of the share at 1000 N m (0.349 with the ring held all round), and where the planets sit
  against them does not change the pairs' means (planets 1 and 3 are 5.5 spline pitches apart). The coarse meshes the
  study used give the fine ones' share within 0.001.

## Does not model (yet)

Friction between teeth, dynamics (the analysis is quasi-static), shaft, bearing, pin and carrier compliance (bores
are held rigidly; planets turn on rigid pins; a ring is held at its outer surface), plasticity. Contact is
geometrically linear about the settled pose: the forces act along the undeformed flanks' normals, so the work they
would do on flanks turned by the teeth's bending is left out, and a tooth's corner is an edge as sharp as the outline
(no edge break, no tip relief unless given). Lead crowning is a parabolic gap across the face (`lead_crown_m`).
