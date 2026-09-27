#!/bin/sh
# lab_films.sh - the README's lab films, all in the lab's shared style (src/lab/style.h): one room, one palette.
# Each film comes from a result made by build/labrun from the named scenario; RUNS is where those results are.
#   RUNS=/tmp/opai_runs sh tools/showcase/lab_films.sh [name ...]
set -e
cd "$(dirname "$0")/../.."
RUNS=${RUNS:-/tmp/opai_runs}
OUT=docs/media/lab
mkdir -p /tmp/lab_films
film() { # name result gif_delay size labfilm-options...
    name=$1 res=$2 delay=$3 size=$4; shift 4
    [ -f "$RUNS/$res" ] || { echo "skip $name: $RUNS/$res missing (build/labrun the scenario first)"; return; }
    dir=$(mktemp -d /tmp/lab_films/film.XXXXXX) # a fresh directory each time: nothing is ever deleted here
    ./build/labfilm "$RUNS/$res" --out "$dir/f" --size "$size" "$@" > /dev/null
    swift tools/encode_gif.swift "$OUT/$name.gif" "$delay" "$dir"/f/frame_*.png > /dev/null
    echo "$name: $(ls -la "$OUT/$name.gif" | awk '{print $5}') bytes (frames in $dir)"
}
sel="$*"
on() { [ -z "$sel" ] || echo " $sel " | grep -q " $1 "; }
on capsule-mach6   && film capsule-mach6 capsule_l4.lab 0.08 960x540 --field mach --range 0 8 --mirror --mesh-below --solid solid --azim -40 --elev 30 --title "Capsule at Mach 6, 30 km" --unit 1
on double-mach     && film double-mach double_mach_reflection.lab 0.08 960x540 --field rho --solid solid --first 4 --zoom 1.5 --center 2.3 1.2 --azim -80 --elev 62 --title "Mach 10 shock on a 30 degree wedge" --unit "(dimensionless)"
on helium-bubble   && film helium-bubble shock_helium_cylinder.lab 0.12 720x405 --field rho --mirror --every 2 --zoom 2.2 --center 0.115 0 --title "Mach 1.22 shock through a helium cylinder" --unit kg/m^3
on thruster-nozzle && film thruster-nozzle thruster_nozzle.lab 0.08 960x540 --field mach --range 0 3 --mirror --solid solid --title "Cold-gas nozzle, 10 bar into 1 bar" --unit 1
on cylinder-re105  && film cylinder-re105 cylinder_re105.lab 0.1 800x450 --field vorticity --range -4 4 --solid solid --radius 0.8 --title "Cylinder at Re 105: the Karman street in dye" --unit 1/s
on taylor-copper   && film taylor-copper taylor.lab 0.08 800x450 --field ep --mesh --title "Taylor test: OFHC copper at 190 m/s" --unit 1
on sphere-plate    && film sphere-plate plate.lab 0.08 800x450 --field ep --view bottom --title "Copper sphere at 800 m/s through a copper plate" --unit 1
on whipple-shield  && film whipple-shield whipple.lab 0.12 720x405 --field speed --range 0 6500 --every 2 --azim 35 --elev 18 --title "Whipple shield, 6.5 km/s" --unit m/s
on crater          && film crater crater.lab 0.12 640x360 --field speed --range 0 5000 --every 2 --azim 35 --elev 38 --zoom 1.9 --title "Crater, 5 km/s" --unit m/s
on shoebox-hall    && film shoebox-hall hall2.lab 0.08 800x450 --field p --range -0.1 0.1 --title "A pulse from the stage of a concert hall" --unit Pa
on radar-pulse     && film radar-pulse radar.lab 0.08 800x450 --field ez --range -0.2 0.2 --solid material --title "A 3 GHz radar pulse meets a sphere and a cylinder" --unit V/m
on wing-10         && film wing-10 naca0012_re5300_aoa10.lab 0.1 640x360 --field vorticity --range -60 60 --solid solid --zoom 2.4 --center 0.21 0.16 --azim -90 --elev 58 --title "NACA 0012 at 10 degrees, Re 5300" --unit 1/s
on wing-20         && film wing-20 naca0012_re5300_aoa20.lab 0.1 640x360 --field vorticity --range -60 60 --solid solid --zoom 2.4 --center 0.21 0.16 --azim -90 --elev 58 --title "NACA 0012 at 20 degrees: stalled" --unit 1/s
on dam-break       && film dam-break dam_break.lab 0.1 640x360 --every 2 --field speed --range 0 3 --azim -60 --elev 35 --title "A dam breaks and the surge hits a block" --unit m/s
on sloshing-tank   && film sloshing-tank sloshing_tank.lab 0.08 560x315 --every 2 --field speed --range 0 0.16 --azim -70 --elev 25 --title "Water sloshes in a tank" --unit m/s
on motor-heats     && film motor-heats pm_motor_drive.lab 0.1 640x360 --field T --range 313 321 --lines A 24 --zoom 1.3 --azim -70 --elev 55 --title "A motor spins up and heats" --unit K
on black-hole      && film black-hole black_hole.lab 0.08 720x405 --title "Light bends around a black hole"
# kept as made on 2026-09-25 (space, already on the room's background):
# on apophis-flyby   && film apophis-flyby flyby.lab 0.08 800x450 --labels --true-size --focus 3 --span 1.0e8 --title "Apophis passes the Earth, 13 April 2029"
