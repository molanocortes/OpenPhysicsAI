#!/usr/bin/env python3
"""Initial conditions for the orbit solver from NASA JPL: state vectors from the Horizons system and GM values of the
DE440 ephemeris (https://ssd.jpl.nasa.gov/astro_par.html, read 2026-09-25).

    python3 tools/horizons.py solar_system --epoch 2000-01-01 --out examples/lab/solar_system_2000.json
    python3 tools/horizons.py vectors 499 --epoch 2020-01-01          # one body's state, barycentric, km and km/s

Vectors are barycentric (the Solar System barycentre, 500@0), in the ecliptic and mean equinox of J2000, TDB. The
query and the date it was made are written into the scenario's provenance. Standard library only; needs the network.
"""
import argparse
import datetime
import json
import pathlib
import subprocess
import urllib.parse

API = 'https://ssd.jpl.nasa.gov/api/horizons.api'
# DE440 GM values, km^3/s^2, as printed on https://ssd.jpl.nasa.gov/astro_par.html
GM = {'sun': 132712440041.279419, 'mercury': 22031.868551, 'venus': 324858.592000, 'earth': 398600.435507,
      'moon': 4902.800118, 'mars': 42828.375816, 'jupiter': 126712764.100000, 'saturn': 37940584.841800,
      'uranus': 5794556.400000, 'neptune': 6836527.100580, 'pluto': 975.500000}
# Horizons identifiers: the Sun, Mercury and Venus themselves, the Earth and the Moon apart, the other planets as the
# barycentres of their systems (their GM is the system's)
IDS = {'sun': '10', 'mercury': '199', 'venus': '299', 'earth': '399', 'moon': '301', 'mars': '4', 'jupiter': '5',
       'saturn': '6', 'uranus': '7', 'neptune': '8', 'pluto': '9'}
RADIUS_KM = {'sun': 695700, 'mercury': 2440.5, 'venus': 6051.8, 'earth': 6371.0, 'moon': 1737.4, 'mars': 3389.5,
             'jupiter': 69911, 'saturn': 58232, 'uranus': 25362, 'neptune': 24622, 'pluto': 1188.3}


def query(command, epoch):
    t0 = datetime.date.fromisoformat(epoch)
    t1 = t0 + datetime.timedelta(days=1)
    params = {'format': 'text', 'COMMAND': "'%s'" % command, 'OBJ_DATA': "'NO'", 'MAKE_EPHEM': "'YES'", 'EPHEM_TYPE': "'VECTORS'",
              'CENTER': "'500@0'", 'START_TIME': "'%s'" % t0.isoformat(), 'STOP_TIME': "'%s'" % t1.isoformat(), 'STEP_SIZE': "'1 d'",
              'VEC_TABLE': "'2'", 'REF_PLANE': "'ECLIPTIC'", 'REF_SYSTEM': "'ICRF'", 'OUT_UNITS': "'KM-S'", 'CSV_FORMAT': "'YES'",
              'VEC_LABELS': "'NO'", 'TIME_TYPE': "'TDB'"}
    url = API + '?' + urllib.parse.urlencode(params)
    # the system curl (it verifies certificates against the system store, which this Python may not have)
    text = subprocess.run(['curl', '-sS', '--max-time', '60', url], capture_output=True, text=True, check=True).stdout
    if '$$SOE' not in text:
        raise SystemExit('Horizons gave no vectors for %s:\n%s' % (command, text[-800:]))
    first = text.split('$$SOE')[1].split('$$EOE')[0].strip().splitlines()[0]
    f = [x.strip() for x in first.split(',')]
    # JDTDB, calendar, X, Y, Z, VX, VY, VZ
    return [float(v) for v in f[2:5]], [float(v) for v in f[5:8]], url


def cmd_vectors(args):
    x, v, url = query(args.command, args.epoch)
    print(json.dumps({'command': args.command, 'epoch_tdb': args.epoch, 'position_km': x, 'velocity_km_s': v, 'query': url}))


def cmd_solar_system(args):
    bodies = []
    queries = []
    for name in ['sun', 'mercury', 'venus', 'earth', 'moon', 'mars', 'jupiter', 'saturn', 'uranus', 'neptune', 'pluto']:
        x, v, url = query(IDS[name], args.epoch)
        bodies.append({'name': name, 'gm_km3_s2': GM[name], 'radius_km': RADIUS_KM[name], 'position_km': x, 'velocity_km_s': v})
        queries.append(url)
    for extra in args.extra or []:
        name, cmd = extra.split('=')
        x, v, url = query(cmd, args.epoch)
        bodies.append({'name': name, 'gm_km3_s2': 0.0, 'radius_km': 0.2, 'position_km': x, 'velocity_km_s': v})
        queries.append(url)
    sc = {'domain': 'orbit', 'title': args.title or 'Solar System from %s' % args.epoch,
          'provenance': {'vectors': 'JPL Horizons, barycentric (500@0), ecliptic J2000, TDB, epoch %s, retrieved %s' % (args.epoch, datetime.date.today().isoformat()),
                         'gm': 'DE440, https://ssd.jpl.nasa.gov/astro_par.html (read 2026-09-25)',
                         'radii': 'mean radii for drawing only',
                         'queries': queries},
          'epoch_tdb': args.epoch, 'relativity': True, 'stages': 6, 'step_days': args.step_days,
          'bodies': bodies,
          'run': {'end_days': args.days, 'frames': args.frames, 'paths': True}}
    p = pathlib.Path(args.out)
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(json.dumps(sc, indent=1) + '\n')
    print('wrote', p)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    a = sub.add_parser('vectors')
    a.add_argument('command')
    a.add_argument('--epoch', required=True)
    a = sub.add_parser('solar_system')
    a.add_argument('--epoch', required=True)
    a.add_argument('--out', required=True)
    a.add_argument('--title')
    a.add_argument('--days', type=float, default=730)
    a.add_argument('--frames', type=int, default=73)
    a.add_argument('--step-days', type=float, default=0.5)
    a.add_argument('--extra', action='append', help='NAME=HORIZONS_COMMAND for a massless extra body, e.g. apophis="99942;"')
    args = ap.parse_args()
    {'vectors': cmd_vectors, 'solar_system': cmd_solar_system}[args.cmd](args)


if __name__ == '__main__':
    main()
