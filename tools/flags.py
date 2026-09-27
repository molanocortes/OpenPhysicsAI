#!/usr/bin/env python3
"""Flags: measurements of the real world that a simulator must predict (flags/README.md).

  tools/flags.py seal DRAFT.json            maintainer: move answers into the sealed store, publish the commitment
  tools/flags.py run --entry NAME [--flag ID] run an entry on every open flag (or one, merged), write its predictions
  tools/flags.py verify --entry NAME        rerun and compare with the stored predictions (reproducibility)
  tools/flags.py score                      score every entry against the sealed answers, log the attempts, draw the boards
  tools/flags.py board                      draw the boards from flags/scores.json and flags/attempts.jsonl: the front
                                            page's tables, every flag's page, flags/LEADERBOARD.md and the SVG cards
  tools/flags.py check-commitments          the sealed files present match the published hashes
  tools/flags.py judge --predictions P --entry-json E --author LOGIN --sha SHA
                                            CI: score one pull request's rerun, log the attempt, write the comment

Thirteen flags carry a number (01 to 13) and a tier: flag, semi holy grail (nature scores it on a known date) or holy
grail (beyond the reach of today's simulators). Trials are smaller challenges, each leading to one of the thirteen.

A board credits people: the GitHub name of the challenger, the open-source libraries the entry is built on and the AI
model that helped, all declared in the entry's entry.json. A flag is captured when a challenger's banded score reaches
CAPTURE; the first capture is kept for good.

Sealed store: $OPAI_SEALED or ~/.openphysicsai/sealed/, one <flag id>.json each; never inside the repository. In CI
the sealed files come from the repository secret OPAI_SEALED_JSON ({flag id: sealed file}), never from a pull request.
Standard library only.
"""
import argparse
import datetime
import hashlib
import json
import math
import os
import pathlib
import random
import re
import secrets
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
FLAGS = ROOT / 'flags'
ENTRIES = FLAGS / 'entries'
ATTEMPTS = FLAGS / 'attempts.jsonl'
SCORE_FLOOR = 0.02  # the scoring uncertainty is never below 2 per cent
BAND = 5            # published flag scores are rounded to bands of this many points
CAPTURE = 90        # a banded score at or above this captures a flag (clears a trial)
FONT = "-apple-system, 'Segoe UI', Inter, Helvetica, Arial, sans-serif"

TIERS = {
    'flag': {'name': 'Flag', 'label': 'FLAG', 'weight': 1, 'accent': '#5cc8ff', 'bg0': '#06121f', 'bg1': '#0e2944',
             'core0': '#1a4470', 'core1': '#040a14'},
    'semi-holy-grail': {'name': 'Semi holy grail', 'label': 'SEMI HOLY GRAIL', 'weight': 2, 'accent': '#dfe7f2',
                        'bg0': '#0e1117', 'bg1': '#2b3445', 'core0': '#444d5e', 'core1': '#05070c'},
    'holy-grail': {'name': 'Holy grail', 'label': 'HOLY GRAIL', 'weight': 3, 'accent': '#f2c14e', 'bg0': '#130c02',
                   'bg1': '#3d2b08', 'core0': '#6b4e12', 'core1': '#0b0702'},
    'trial': {'name': 'Trial', 'label': 'TRIAL', 'weight': 0, 'accent': '#8fd694', 'bg0': '#081a10',
                'bg1': '#12301f', 'core0': '#1c4d2e', 'core1': '#030a05'},
}
MEDALS = ['\U0001F947', '\U0001F948', '\U0001F949']  # gold, silver and bronze medals


def sealed_dir():
    return pathlib.Path(os.environ.get('OPAI_SEALED', pathlib.Path.home() / '.openphysicsai' / 'sealed'))


def canonical(obj):
    return json.dumps(obj, sort_keys=True, separators=(',', ':'), ensure_ascii=False).encode()


def load_flags():
    out = []
    for p in sorted(FLAGS.glob('*/flag.json')):
        f = json.loads(p.read_text())
        f['_dir'] = p.parent
        out.append(f)
    return out


def scorable(f):
    """A flag can be run and scored once its cases are posed and its answers sealed."""
    return bool(f.get('cases')) and bool(f.get('sealed'))


def main_flags(flags):
    return sorted([f for f in flags if f.get('number')], key=lambda f: f['number'])


def warmups(flags):
    """The trials in their order, T1 to T4."""
    return sorted([f for f in flags if f.get('tier') == 'trial'], key=lambda f: (int(f.get('label', 'T99')[1:] or 99), f['id']))


# ---------------------------------------------------------------------------------------------------- sealing, running

def cmd_seal(args):
    draft = json.loads(pathlib.Path(args.draft).read_text())
    fid = draft['id']
    fpath = FLAGS / fid / 'flag.json'
    flag = json.loads(fpath.read_text())
    cases = {c['case'] for c in flag['cases']}
    outputs = {o['name'] for o in flag['outputs']}
    if set(draft['answers']) != cases:
        sys.exit('seal: the answers cover %s, the flag has %s' % (sorted(draft['answers']), sorted(cases)))
    for c, a in draft['answers'].items():
        if set(a) != outputs:
            sys.exit('seal: case %s answers %s, the flag asks %s' % (c, sorted(a), sorted(outputs)))
        for k, v in a.items():
            if not (isinstance(v, list) and len(v) == 2 and v[1] > 0):
                sys.exit('seal: %s.%s must be [value, relative or absolute uncertainty > 0]' % (c, k))
    if not draft.get('source', {}).get('citation'):
        sys.exit('seal: a source with a citation is required')
    draft.setdefault('salt', secrets.token_hex(16))
    d = sealed_dir()
    d.mkdir(parents=True, exist_ok=True)
    target = d / (fid + '.json')
    target.write_text(json.dumps(draft, indent=1) + '\n')
    os.chmod(target, 0o600)
    h = hashlib.sha256(canonical(draft)).hexdigest()
    flag['sealed'] = {'sha256': h, 'sealed_on': datetime.date.today().isoformat()}
    fpath.write_text(json.dumps(flag, indent=1, ensure_ascii=False) + '\n')
    print('sealed %s: %s (commitment %s)' % (fid, target, h))
    if args.remove_draft:
        os.remove(args.draft)


def cmd_export(args):
    """The sealed store as one JSON object, {flag id: sealed file}, checked against the commitments, for the repository
    secret OPAI_SEALED_JSON: python3 tools/flags.py export-sealed | gh secret set OPAI_SEALED_JSON"""
    flags = [f for f in load_flags() if scorable(f)]
    sys.stdout.write(json.dumps(load_sealed(flags), sort_keys=True, separators=(',', ':')))


def cmd_check(args):
    bad = 0
    for f in load_flags():
        if not f.get('sealed'):
            continue
        p = sealed_dir() / (f['id'] + '.json')
        want = f['sealed']['sha256']
        if not p.exists():
            print('%-28s no sealed file here' % f['id'])
            continue
        got = hashlib.sha256(canonical(json.loads(p.read_text()))).hexdigest()
        ok = got == want
        bad += not ok
        print('%-28s %s' % (f['id'], 'matches its commitment' if ok else 'DOES NOT MATCH %s' % want))
    sys.exit(1 if bad else 0)


def run_entry(name, only=None):
    edir = ENTRIES / name
    entry = json.loads((edir / 'entry.json').read_text())
    preds = {}
    for f in load_flags():
        cmd = entry.get('commands', {}).get(f['id'])
        if not cmd or not f.get('cases') or (only and f['id'] != only):
            continue
        cases = [dict(c, practice=False) for c in f['cases']] + [dict(c, practice=True) for c in f.get('practice', [])]
        for c in cases:
            c.pop('answers', None)
        env = dict(os.environ, FLAG_ID=f['id'], FLAG_CASES=json.dumps(cases), FLAG_DIR=str(f['_dir']))
        print('running %s on %s ...' % (name, f['id']), flush=True)
        r = subprocess.run(cmd, shell=True, cwd=ROOT, env=env, capture_output=True, text=True)
        if r.returncode != 0:
            print(r.stderr[-2000:])
            sys.exit('run: %s failed on %s' % (name, f['id']))
        last = [l for l in r.stdout.strip().splitlines() if l.startswith('{')]
        if not last:
            sys.exit('run: %s printed no JSON predictions for %s' % (name, f['id']))
        preds[f['id']] = json.loads(last[-1])
    commit = subprocess.run(['git', 'rev-parse', '--short', 'HEAD'], cwd=ROOT, capture_output=True, text=True).stdout.strip()
    dirty = subprocess.run(['git', 'diff', '--quiet', 'HEAD', '--', 'src', 'tools', 'flags'], cwd=ROOT).returncode != 0
    return {'entry': name, 'commit': commit + (' with local changes' if dirty else ''),
            'date': datetime.date.today().isoformat(), 'predictions': preds}


def cmd_run(args):
    out = run_entry(args.entry, args.flag)
    p = ENTRIES / args.entry / 'predictions.json'
    if args.flag and p.exists():  # one flag: merged into the predictions already stored, under the new commit
        old = json.loads(p.read_text())
        old['predictions'].update(out['predictions'])
        old['commit'], old['date'] = out['commit'], out['date']
        out = old
    p.write_text(json.dumps(out, indent=1) + '\n')
    print('wrote', p.relative_to(ROOT))
    # the practice cases can be checked by anyone: their answers are public
    for f in load_flags():
        for c in f.get('practice', []):
            got = out['predictions'].get(f['id'], {}).get(c['case'])
            if got:
                for k, v in c['answers'].items():
                    print('  practice %s %s %s: %.4g against %.4g (%+.1f %%)' % (f['id'], c['case'], k, got[k], v, 100 * (got[k] - v) / v))


def cmd_verify(args):
    stored = json.loads((ENTRIES / args.entry / 'predictions.json').read_text())
    again = run_entry(args.entry)
    worst = 0.0
    for fid, cases in stored['predictions'].items():
        for c, outs in cases.items():
            for k, v in outs.items():
                w = again['predictions'][fid][c][k]
                worst = max(worst, abs(w - v) / max(abs(v), 1e-30))
    print('largest relative difference between the stored predictions and the rerun: %.2e' % worst)
    sys.exit(0 if worst < 1e-6 else 1)


# ---------------------------------------------------------------------------------------------------- scoring

def case_score(p, m, s, metric):
    s = max(s, SCORE_FLOOR) if metric == 'log_ratio' else s
    if metric == 'log_ratio':
        if p is None or p <= 0 or m <= 0:
            return 0.0
        e = abs(math.log(p / m)) / math.log(1 + s)
    else:
        e = abs(p - m) / s
    return math.exp(-0.5 * e * e)


def credit(entry, name):
    """Who gets the credit for an entry: the challenger, the libraries, the AI model, from its entry.json."""
    return {'who': entry.get('who', name), 'ai': entry.get('ai', 'not stated'), 'libraries': entry.get('libraries', []),
            'baseline': bool(entry.get('baseline', False)), 'open': bool(entry.get('open', False))}


def load_attempts():
    if not ATTEMPTS.exists():
        return []
    return [json.loads(l) for l in ATTEMPTS.read_text().splitlines() if l.strip()]


def load_sealed(flags):
    """The sealed answers of the scorable flags, each checked against its published commitment: from the environment
    variable OPAI_SEALED_JSON when it is set (CI, a repository secret), else from the sealed store."""
    env = os.environ.get('OPAI_SEALED_JSON')
    store = json.loads(env) if env else None
    sealed = {}
    for f in flags:
        if store is not None:
            a = store.get(f['id'])
        else:
            p = sealed_dir() / (f['id'] + '.json')
            a = json.loads(p.read_text()) if p.exists() else None
        if a is None:
            sys.exit('score: the sealed answers of %s are not in %s' % (f['id'], 'OPAI_SEALED_JSON' if env else sealed_dir()))
        if hashlib.sha256(canonical(a)).hexdigest() != f['sealed']['sha256']:
            sys.exit('score: the sealed file of %s does not match its commitment' % f['id'])
        sealed[f['id']] = a
    return sealed


def flag_score(f, got, sealed):
    """One flag's banded score for one entry's predictions on it."""
    vals = []
    for c in f['cases']:
        for o in f['outputs']:
            m, s = sealed[f['id']]['answers'][c['case']][o['name']]
            p = got.get(c['case'], {}).get(o['name'])
            vals.append(case_score(p, m, s, f.get('metric', 'log_ratio')) if p is not None else 0.0)
    raw = 100 * sum(vals) / len(vals)
    return int(BAND * round(raw / BAND))


def cmd_score(args):
    flags = [f for f in load_flags() if scorable(f)]
    sealed = load_sealed(flags)
    board, attempts = [], load_attempts()
    seen = {(a['entry'], a['flag'], a['commit']) for a in attempts}
    for pp in sorted(ENTRIES.glob('*/predictions.json')):
        pred = json.loads(pp.read_text())
        entry = json.loads((pp.parent / 'entry.json').read_text())
        cr = credit(entry, pred['entry'])
        flag_scores = {}
        for f in flags:
            got = pred['predictions'].get(f['id'])
            if not got:
                continue
            flag_scores[f['id']] = flag_score(f, got, sealed)
            key = (pred['entry'], f['id'], pred['commit'])
            if key not in seen:  # an attempt is an entry's predictions at one commit; re-scoring them is not another
                seen.add(key)
                attempts.append(dict(cr, date=pred['date'], flag=f['id'], entry=pred['entry'], title=entry.get('title', pred['entry']),
                                     commit=pred['commit'], score=flag_scores[f['id']]))
        board.append(dict(cr, entry=pred['entry'], title=entry.get('title', pred['entry']), commit=pred['commit'], date=pred['date'],
                          flags=flag_scores))
    ATTEMPTS.write_text(''.join(json.dumps(a, ensure_ascii=False, sort_keys=True) + '\n' for a in attempts))
    out = {'scored_on': datetime.date.today().isoformat(), 'capture_at': CAPTURE, 'band': BAND, 'entries': board}
    (FLAGS / 'scores.json').write_text(json.dumps(out, indent=1, ensure_ascii=False) + '\n')
    print('wrote flags/scores.json and flags/attempts.jsonl (%d attempts)' % len(attempts))
    cmd_board(argparse.Namespace(check=False))


# ---------------------------------------------------------------------------------------------------- judging in CI

ENTRY_NAME = re.compile(r'^[a-z0-9][a-z0-9-]{0,38}$')
PROTECTED = ('.github/', 'tools/flags.py', 'flags/attempts.jsonl', 'flags/scores.json')


def clean_predictions(raw, flags):
    """A pull request's predictions read as data: only the scorable flags, their cases and outputs, finite numbers."""
    out = {}
    for f in flags:
        got = raw.get(f['id']) if isinstance(raw, dict) else None
        if not isinstance(got, dict):
            continue
        cases = {}
        for c in f['cases']:
            vals = got.get(c['case'])
            if not isinstance(vals, dict):
                continue
            keep = {o['name']: float(vals[o['name']]) for o in f['outputs']
                    if isinstance(vals.get(o['name']), (int, float)) and not isinstance(vals.get(o['name']), bool)
                    and math.isfinite(vals[o['name']])}
            if keep:
                cases[c['case']] = keep
        if cases:
            out[f['id']] = cases
    return out


def cmd_judge(args):
    """Score one pull request's entry, in the trusted CI workflow on main's code. The predictions and the entry come from
    the pull request's rerun as data and are never executed; the author and the commit come from GitHub itself. Logs the
    attempt, redraws the boards, and writes the comment for the pull request and its label."""
    flags = [f for f in load_flags() if scorable(f)]
    byid = {f['id']: f for f in load_flags()}
    comment, label = [], 'flag: not scored'

    def done():
        pathlib.Path(args.comment).write_text('\n'.join(comment) + '\n')
        pathlib.Path(args.label).write_text(label + '\n')
        print('\n'.join(comment))

    try:
        pred = json.loads(pathlib.Path(args.predictions).read_text()[:2_000_000])
        entry = json.loads(pathlib.Path(args.entry_json).read_text()[:200_000])
        name = str(pred.get('entry', ''))
        assert isinstance(entry, dict)
    except (OSError, ValueError, AssertionError, AttributeError):
        comment.append('**Not scored.** The rerun left no readable `predictions.json` and `entry.json`.')
        return done()
    if not ENTRY_NAME.match(name):
        comment.append('**Not scored.** An entry\'s directory name is lowercase letters, digits and dashes: `%s` is not.' % name[:60])
        return done()
    mine = ENTRIES / name / 'entry.json'
    if mine.exists() and json.loads(mine.read_text()).get('who') != args.author:
        comment.append('**Not scored.** The entry `%s` belongs to @%s. Name yours after your GitHub name: `flags/entries/%s/`.'
                       % (name, json.loads(mine.read_text()).get('who'), args.author.lower()))
        return done()
    today = datetime.date.today().isoformat()
    attempts = load_attempts()
    if any(a['entry'] == name and a['date'] == today for a in attempts):
        comment.append('**Scored once today already.** One scored submission per entry per day keeps the answers from being '
                       'probed; push again tomorrow, or re-run this check then, and it will be scored.')
        label = 'flag: tomorrow'
        return done()
    got = clean_predictions(pred.get('predictions'), flags)
    if not got:
        comment.append('**Not scored.** The rerun predicted no case of an open flag or trial; see the flag\'s `flag.json` '
                       'for the cases and outputs it asks.')
        return done()
    sealed = load_sealed([f for f in flags if f['id'] in got])
    entry['who'] = args.author  # the credit goes to whoever opened the pull request
    cr = credit(entry, name)
    commit = args.sha[:7]
    scores = {fid: flag_score(byid[fid], got[fid], sealed) for fid in got}
    comment += ['### Scored against the real world', '',
                'Rerun from commit `%s` on a clean machine and scored against the sealed measurements. Scores are out '
                'of 100, in bands of %d; %d captures a flag or clears a trial.' % (commit, BAND, CAPTURE), '',
                '| | Challenge | Your score | Record before | |', '| :---: | :--- | :---: | :---: | :--- |']
    news = []
    for fid, sc in sorted(scores.items(), key=lambda kv: byid[kv[0]].get('label') or '%02d' % byid[kv[0]].get('number', 99)):
        f = byid[fid]
        prior = [a['score'] for a in attempts if a['flag'] == fid and not a.get('baseline')]
        best = max(prior) if prior else None
        captured = best is not None and best >= CAPTURE
        what = ''
        if sc >= CAPTURE and not captured and not cr['baseline']:
            what = '**first capture**' if f.get('tier') != 'trial' else '**first to clear it**'
            news.append('capture')
        elif best is None or sc > best:
            what = 'new record' if not cr['baseline'] else 'baseline'
            news.append('record')
        tag = f.get('label') or '%02d' % f.get('number', 0)
        comment.append('| %s | [%s](%s) | **%d** | %s | %s |' % (tag, f['title'], 'https://github.com/%s/blob/main/flags/%s/FLAG.md'
                       % (os.environ.get('GITHUB_REPOSITORY', 'molanocortes/OpenPhysicsAI'), fid), sc,
                       best if best is not None else 'none', what))
        key = (name, fid, commit)
        if key not in {(a['entry'], a['flag'], a['commit']) for a in attempts}:
            attempts.append(dict(cr, date=today, flag=fid, entry=name, title=entry.get('title', name), commit=commit, score=sc))
    ATTEMPTS.write_text(''.join(json.dumps(a, ensure_ascii=False, sort_keys=True) + '\n' for a in attempts))
    sp = FLAGS / 'scores.json'
    board = json.loads(sp.read_text()) if sp.exists() else {'entries': []}
    board['entries'] = [e for e in board.get('entries', []) if e.get('entry') != name] + [
        dict(cr, entry=name, title=entry.get('title', name), commit=commit, date=today, flags=scores)]
    board.update(scored_on=today, capture_at=CAPTURE, band=BAND)
    sp.write_text(json.dumps(board, indent=1, ensure_ascii=False) + '\n')
    cmd_board(argparse.Namespace(check=False))
    touched = [l.strip() for l in pathlib.Path(args.files).read_text().splitlines() if l.strip()] if args.files else []
    odd = [t for t in touched if t.startswith(PROTECTED) or re.match(r'flags/[^/]+/flag\.json$', t)
           or (t.startswith('flags/entries/') and not t.startswith('flags/entries/%s/' % name))]
    comment += ['', 'The attempt is logged on the boards under @%s. ' % args.author +
                ('**Congratulations:** a maintainer reads the code of every capture and record before it is merged into '
                 'the lab for everyone.' if news else 'Improve it and push again: every push is rerun and scored, once a day.')]
    if odd:
        comment += ['', 'This pull request also changes files that only maintainers change: %s. Keep it to your entry '
                    'and the lab\'s code and it can be merged.' % ', '.join('`%s`' % t for t in odd[:8])]
    label = 'flag: capture' if 'capture' in news else 'flag: record' if 'record' in news else 'flag: scored'
    return done()


# ---------------------------------------------------------------------------------------------------- the boards

def flag_stats(fid, attempts):
    """The board of one flag: the best attempt of each challenger, ranked, and the baselines apart."""
    rows = [a for a in attempts if a['flag'] == fid]
    best = {}
    for a in rows:
        key = 'baseline:' + a['entry'] if a.get('baseline') else a['who']
        b = best.get(key)
        if b is None or a['score'] > b['score'] or (a['score'] == b['score'] and a['date'] < b['date']):
            best[key] = a
    ranked = sorted([a for a in best.values() if not a.get('baseline')], key=lambda a: (-a['score'], a['date'], a['who']))
    baselines = sorted([a for a in best.values() if a.get('baseline')], key=lambda a: -a['score'])
    captures = sorted([a for a in rows if not a.get('baseline') and a['score'] >= CAPTURE], key=lambda a: a['date'])
    return {'attempts': len(rows), 'challengers': len({a['who'] for a in rows if not a.get('baseline')}), 'ranked': ranked,
            'baselines': baselines, 'record': ranked[0] if ranked else None, 'first': captures[0] if captures else None}


def status_of(f, st):
    """(key, words for a pill, words for a table)"""
    if st['first'] and f.get('status') != 'rebuilding':
        verb = 'CLEARED' if f.get('tier') == 'trial' else 'CAPTURED'
        return 'captured', '%s BY @%s' % (verb, st['first']['who'].upper()), '**%s** by @%s' % (verb.lower(), st['first']['who'])
    s = f.get('status', 'preparing')
    if s == 'rebuilding':
        rec = st['record']
        return 'preparing', 'BEING REBUILT IN 3D', 'being rebuilt in 3D' + ('; record %d stands' % rec['score'] if rec else '')
    if s == 'open':
        return 'open', 'OPEN', 'open'
    if s == 'awaiting':
        return 'awaiting', 'NATURE SCORES IT ON %s' % f.get('event', '').upper(), 'nature scores it on %s' % f.get('event', '')
    return 'preparing', 'IN PREPARATION', 'in preparation'


def difficulty(f):
    d = f.get('difficulty', 0)
    return '∞' if d > 5 else '★' * d + '☆' * (5 - d)


def esc(s):
    return str(s).replace('&', '&amp;').replace('<', '&lt;').replace('>', '&gt;')


def who_link(who):
    return '[@%s](https://github.com/%s)' % (who, who)


def libs(a):
    return ', '.join(a.get('libraries') or []) or 'not stated'


def credit_line(a):
    """What an entry was built on and with which AI model: 'OpenPhysicsAI, using Claude Opus 5.5'."""
    ai = a.get('ai') or 'not stated'
    return libs(a) if ai.lower() in ('none', 'no', '') else '%s, using %s' % (libs(a), ai)


def credit_cell(a, lead=False):
    who = who_link(a['who'])
    return '%s<br><sub>%s</sub>' % ('**%s**' % who if lead else who, esc(credit_line(a)))


def board_md(f, st, top=None, rel=''):
    """The markdown board of one flag; rel is the path from the page it goes on to the repository's root."""
    lines = []
    rec = st['record']
    stat = ['**Attempts** %d' % st['attempts'], '**Challengers** %d' % st['challengers'],
            '**Record** %s' % ('%d by @%s' % (rec['score'], rec['who']) if rec else 'none yet'),
            '**First %s** %s' % ('clear' if f.get('tier') == 'trial' else 'capture',
                                 '@%s, %s' % (st['first']['who'], st['first']['date']) if st['first'] else 'still to be won')]
    lines.append(' · '.join(stat))
    lines.append('')
    ranked = st['ranked'] if top is None else st['ranked'][:top]
    if not ranked and not st['baselines']:
        if f.get('status', 'preparing') == 'preparing':
            lines.append('> **Unclaimed, and opening soon.** This flag is in preparation: its board opens when its cases are published '
                         'and its answers sealed. The first name written here stays in its history for good.')
        else:
            lines.append('> **Unclaimed.** No one has attempted this flag yet. The first name written on this board stays in its history '
                         'for good.')
        return '\n'.join(lines)
    lines += ['| Rank | Challenger | Score | Date |', '| :---: | :--- | ---: | :--- |']
    for i, a in enumerate(ranked):
        medal = MEDALS[i] if i < 3 else str(i + 1)
        score = '**%d**' % a['score']
        if i == 0 and a['score'] >= CAPTURE:
            score = '**%d** \U0001F3C6' % a['score']
        lines.append('| %s | %s | %s | %s |' % (medal, credit_cell(a, lead=i == 0), score, a['date']))
    if not ranked:
        lines.append('| %s | *no challenger yet: be the first* | | |' % MEDALS[0])
    for a in st['baselines']:
        lines.append('| baseline | *%s*<br><sub>%s, for reference, not ranked</sub> | %d | %s |' % (esc(a.get('title', a['entry'])), esc(libs(a)), a['score'], a['date']))
    if top is not None and len(st['ranked']) > top:
        lines.append('')
        lines.append('%d more on the [full board](%sflags/%s/FLAG.md#leaderboard).' % (len(st['ranked']) - top, rel, f['id']))
    return '\n'.join(lines)


def hall(mains, attempts):
    """Challengers across the thirteen: flags captured, then power (score times the tier's weight, best per flag)."""
    people = {}
    for f in mains:
        w = TIERS[f['tier']]['weight']
        for a in flag_stats(f['id'], attempts)['ranked']:
            p = people.setdefault(a['who'], {'who': a['who'], 'captured': 0, 'power': 0, 'flags': 0, 'since': a['date'],
                                             'ai': set(), 'libraries': set()})
            p['captured'] += a['score'] >= CAPTURE
            p['power'] += w * a['score']
            p['flags'] += 1
            p['since'] = min(p['since'], a['date'])
            p['ai'].add(a.get('ai', 'not stated'))
            p['libraries'].update(a.get('libraries') or [])
    return sorted(people.values(), key=lambda p: (-p['captured'], -p['power'], p['since']))


def max_power(mains):
    return sum(100 * TIERS[f['tier']]['weight'] for f in mains)


def hall_md(mains, attempts, rel=''):
    h = hall(mains, attempts)
    lines = ['<img src="%sflags/podium.svg" alt="The hall of fame: the three challengers who hold the most flags" width="100%%">' % rel, '']
    if not h:
        lines.append('> **No flag has been captured yet, and no name is written here.** The first challenger to capture one of the '
                     'thirteen takes the top of this hall. Power counts every flag attempted: its best score, once for a flag, twice '
                     'for a semi holy grail, three times for a holy grail, out of %d.' % max_power(mains))
        return '\n'.join(lines)
    lines += ['| Rank | Challenger | Flags captured | Power | Flags attempted | Since |',
              '| :---: | :--- | :---: | ---: | :---: | :--- |']
    for i, p in enumerate(h[:10]):
        built = credit_line({'libraries': sorted(p['libraries']), 'ai': ', '.join(sorted(p['ai']))})
        lines.append('| %s | %s<br><sub>%s</sub> | %d | %d / %d | %d | %s |' % (
            MEDALS[i] if i < 3 else str(i + 1), ('**%s**' if i == 0 else '%s') % who_link(p['who']), esc(built), p['captured'], p['power'],
            max_power(mains), p['flags'], p['since']))
    return '\n'.join(lines)


def summary_md(mains, attempts):
    lines = ['| # | Flag | Difficulty | Attempts | Record | Status |', '| :---: | :--- | :---: | :---: | :--- | :--- |']
    for f in mains:
        st = flag_stats(f['id'], attempts)
        rec = st['record']
        tier = TIERS[f['tier']]['name'].lower()
        if f['tier'] == 'holy-grail':
            tier = '<b>%s</b>' % tier
        lines.append('| %02d | **[%s](#flag-%02d)**<br><sub>%s · %s</sub> | %s | %d | %s | %s |' % (
            f['number'], f['title'], f['number'], f['stone'], tier, difficulty(f), st['attempts'],
            '**%d** by @%s' % (rec['score'], rec['who']) if rec else '*unclaimed*', status_of(f, st)[2]))
    return '\n'.join(lines)


def stats_line(mains, warms, attempts):
    caught = sum(1 for f in mains if flag_stats(f['id'], attempts)['first'])
    n_att = sum(flag_stats(f['id'], attempts)['attempts'] for f in mains)
    people = {a['who'] for a in attempts if not a.get('baseline') and any(a['flag'] == f['id'] for f in mains)}
    cleared = sum(1 for f in warms if flag_stats(f['id'], attempts)['first'])
    return ('**%d flags** · **%d captured** · **%d attempts** · **%d challengers** · trials: %d, %d cleared' %
            (len(mains), caught, n_att, len(people), len(warms), cleared))


def warmups_md(warms, mains, attempts, rel=''):
    num = {f['id']: f for f in mains}
    lines = ['| | Trial | Best | Holder | Status |', '| :---: | :--- | ---: | :--- | :--- |']
    for f in warms:
        st = flag_stats(f['id'], attempts)
        rec = st['record']
        to = num.get(f.get('leads_to'))
        status = 'rebuilding in 3D' if f.get('status') == 'rebuilding' else status_of(f, st)[2]
        lines.append('| %s | **[%s](%sflags/%s/FLAG.md)**<br><sub>leads to %s</sub> | %s | %s | %s |' % (
            f.get('label', ''), f['title'], rel, f['id'], 'flag %02d, %s' % (to['number'], to['title'].lower() if not to['title'].startswith(('Apophis', 'The Sun')) else to['title']) if to else 'no flag',
            '**%d**' % rec['score'] if rec else '·', credit_cell(rec) if rec else '*unclaimed*', status))
    return '\n'.join(lines)


def replace_block(text, name, block):
    a, b = '<!-- %s:begin -->' % name, '<!-- %s:end -->' % name
    if a not in text or b not in text:
        return text, False
    # blank lines around the block: a table or a quote must end before the closing marker
    return text[:text.index(a)] + a + '\n\n' + block.strip('\n') + '\n\n' + text[text.index(b):], True


# ---------------------------------------------------------------------------------------------------- pictures

# ------------------------------------------------------------------------------------------------ the flags' glyphs
# Each flag has a small animated drawing of its phenomenon and a colour of its own, like the stones of the legend. A
# glyph is drawn in a box from -50 to 50 around the origin; the caller places and scales it. `u` makes the ids of its
# gradients and paths unique, since the hero carries all of them in one document.


STONE = {'car-wake': '#5ce1e6', 'reentry-fire-ii': '#ff8c42', 'jet-flame': '#ffc145', 'drop-splash': '#4fb3ff',
         'metal-printing': '#ff6a3d', 'metal-fracture': '#ff5c5c', 'radar-almond': '#7cffa4', 'spark-streamer': '#b99cff',
         'apophis-2029': '#d5dcea', 'corona-2027': '#fff1c9', 'fusion-shot': '#ff6ad5', 'hurricane-otis': '#a9e4ff',
         'heartbeat': '#ff4d6d', 'flow-cylinder-shedding': '#5ce1e6', 'gas-sphere-bowshock': '#ff8c42',
         'gas-shock-bubble': '#4fb3ff', 'orbit-planets-2020': '#d5dcea'}
INK = '#f3f6fb'


def _pts(ps):
    return ' '.join('%.1f %.1f' % p for p in ps)


def _spiral(x, y, r0, turns, sign, n=26, a0=0.0):
    ps = []
    for i in range(n + 1):
        s = i / n
        a = a0 + sign * 2 * math.pi * turns * s
        r = r0 * (1 - 0.78 * s)
        ps.append((x + r * math.cos(a), y + r * math.sin(a)))
    return 'M ' + ' L '.join('%.1f %.1f' % p for p in ps)


def _drift(inner, dx, dur, begin):
    """A group that drifts dx to the right and fades, then starts again: flow leaving a body."""
    return ('<g opacity="0">%s<animateTransform attributeName="transform" type="translate" from="0 0" to="%d 0" dur="%.1fs" begin="%.2fs" '
            'repeatCount="indefinite"/><animate attributeName="opacity" values="0;1;1;0" keyTimes="0;0.15;0.6;1" dur="%.1fs" begin="%.2fs" '
            'repeatCount="indefinite"/></g>' % (inner, dx, dur, begin, dur, begin))


def _flowlines(ys, x0, x1, col, dur=1.2, op=0.55):
    out = []
    for k, y in enumerate(ys):
        out.append('<line x1="%d" y1="%d" x2="%d" y2="%d" stroke="%s" stroke-width="2" stroke-linecap="round" stroke-dasharray="5 7" opacity="%.2f">'
                   '<animate attributeName="stroke-dashoffset" from="24" to="0" dur="%.1fs" repeatCount="indefinite"/></line>' % (x0, y, x1, y, col, op, dur))
    return out


def g_car(u, c):
    wake = ''.join('<path d="%s" fill="none" stroke="%s" stroke-width="2.8" stroke-linecap="round"/>' % (_spiral(31, y, 8.5, 0.85, s), c)
                   for y, s in ((-5, -1), (9, 1)))
    out = _flowlines((-24, -34), -48, 44, c, op=0.45)
    out += ['<line x1="-47" y1="17" x2="47" y2="17" stroke="%s" stroke-width="2" opacity="0.35"/>' % INK,
            '<path d="M -42 9 L -42 -3 Q -42 -12 -33 -12 L 6 -12 L 22 -5 L 22 9 Z" fill="%s"/>' % INK,
            '<path d="M -31 9 L -31 17 M 12 9 L 12 17" stroke="%s" stroke-width="3" stroke-linecap="round"/>' % INK,
            '<path d="M 6 -13 Q 26 -14 44 -6" fill="none" stroke="%s" stroke-width="2" stroke-linecap="round" stroke-dasharray="4 5" opacity="0.8">'
            '<animate attributeName="stroke-dashoffset" from="18" to="0" dur="0.9s" repeatCount="indefinite"/></path>' % c,
            _drift(wake, 18, 2.4, 0), _drift(wake, 18, 2.4, 1.2)]
    return out


def g_reentry(u, c):
    return ['<defs><radialGradient id="%sh" cx="30%%" cy="50%%" r="70%%"><stop offset="0" stop-color="#fff6d8"/><stop offset="0.45" stop-color="%s"/>'
            '<stop offset="1" stop-color="%s" stop-opacity="0"/></radialGradient></defs>' % (u, c, c)] + _flowlines((-36, -18, 18, 36), -50, -30, INK, 0.8, 0.5) + [
        '<g transform="rotate(-10)">',
        '<path d="M 6 -46 Q -58 0 6 46 L 0 30 Q -24 0 0 -30 Z" fill="url(#%sh)" opacity="0.9"><animate attributeName="opacity" values="0.65;1;0.65" dur="1.6s" '
        'repeatCount="indefinite"/></path>' % u,
        '<path d="M 6 -46 Q -58 0 6 46" fill="none" stroke="%s" stroke-width="2.4"/>' % c,
        '<path d="M -8 -24 Q -20 0 -8 24 L 20 9 Q 24 8 24 4 L 24 -4 Q 24 -8 20 -9 Z" fill="%s"/>' % INK,
        '<path d="M -8 -24 Q -20 0 -8 24" fill="none" stroke="%s" stroke-width="3"/>' % c, '</g>']


def g_flame(u, c):
    flick = '<animateTransform attributeName="transform" type="scale" values="1 1;1.07 0.93;0.94 1.06;1.03 0.97;1 1" dur="1.3s" repeatCount="indefinite"/>'
    return ['<defs><linearGradient id="%sf" x1="0" y1="1" x2="0" y2="0"><stop offset="0" stop-color="#7cc8ff"/><stop offset="0.3" stop-color="%s"/>'
            '<stop offset="1" stop-color="#ff5a2a"/></linearGradient></defs>' % (u, c),
            '<g transform="translate(0 26)"><g>%s<g transform="translate(0 -26)">' % flick,
            '<path d="M -10 26 C -22 6 -12 -22 0 -48 C 12 -22 22 6 10 26 Z" fill="url(#%sf)"/>' % u,
            '<path d="M -4 26 C -10 12 -6 -6 0 -24 C 6 -6 10 12 4 26 Z" fill="#fff7dd" opacity="0.9"/>', '</g></g></g>',
            '<rect x="-6" y="26" width="12" height="22" rx="1.5" fill="%s"/>' % INK,
            '<path d="M -13 30 L -13 48 M 13 30 L 13 48" stroke="%s" stroke-width="3" opacity="0.6"/>' % INK]


def g_splash(u, c):
    drops = ''.join('<circle cx="%d" cy="%d" r="%.1f" fill="%s"><animate attributeName="cy" values="%d;%d;%d" dur="1.8s" begin="%.1fs" repeatCount="indefinite"/>'
                    '</circle>' % (x, y, r, c, y, y - 6, y, b) for x, y, r, b in ((-22, -24, 2.6, 0), (-8, -29, 2.9, 0.3), (8, -29, 2.9, 0.6), (22, -24, 2.6, 0.9)))
    return ['<ellipse cx="0" cy="24" rx="46" ry="5" fill="%s" opacity="0.25"/>' % c,
            '<path d="M -31 24 Q -31 6 -26 -6 L -20 -16 L -13 -5 L -6.5 -18 L 0 -6 L 6.5 -18 L 13 -5 L 20 -16 L 26 -6 Q 31 6 31 24 Z" fill="%s" opacity="0.92"/>' % c,
            '<ellipse cx="0" cy="24" rx="31" ry="6" fill="%s"/>' % INK, drops,
            '<path d="M 0 -50 Q 5 -42 0 -38 Q -5 -42 0 -50 Z" fill="%s" opacity="0.8"/>' % INK]


def g_print(u, c):
    d = 3.2
    mv = '<animateTransform attributeName="transform" type="translate" from="-30 0" to="32 0" dur="%.1fs" repeatCount="indefinite"/>' % d
    return ['<defs><linearGradient id="%st" x1="0" y1="0" x2="1" y2="0"><stop offset="0" stop-color="%s" stop-opacity="0.5"/><stop offset="1" stop-color="%s"/>'
            '</linearGradient></defs>' % (u, INK, c),
            '<path d="M -48 16 L 48 16 L 48 28 L -48 28 Z" fill="%s" opacity="0.28"/>' % INK,
            '<line x1="-48" y1="16" x2="48" y2="16" stroke="%s" stroke-width="2" opacity="0.6"/>' % INK,
            '<rect x="-42" y="12.5" width="12" height="4" rx="2" fill="url(#%st)"><animate attributeName="width" from="12" to="74" dur="%.1fs" repeatCount="indefinite"/></rect>' % (u, d),
            '<g>%s<path d="M -3 -48 L 3 -48 L 0.9 14 L -0.9 14 Z" fill="%s" opacity="0.9"><animate attributeName="opacity" values="0.6;1;0.6" dur="0.4s" repeatCount="indefinite"/></path>'
            '<ellipse cx="0" cy="15" rx="7" ry="3.2" fill="#fff1c4"/><ellipse cx="0" cy="15" rx="11" ry="5" fill="%s" opacity="0.45"/>'
            '<circle cx="4" cy="8" r="1.2" fill="#fff1c4"><animate attributeName="cy" values="12;-2" dur="0.8s" repeatCount="indefinite"/></circle>'
            '<circle cx="-5" cy="6" r="1" fill="#fff1c4"><animate attributeName="cy" values="12;0" dur="1.1s" repeatCount="indefinite"/></circle></g>' % (mv, c, c)]


def g_fracture(u, c):
    crack = 'M -11 1 L -7 3 L -4 0 L 0 2 L 3 -1 L 7 2 L 11 0'
    return ['<path d="M -20 -48 L 20 -48 L 20 -30 Q 11 -26 11 -17 L 11 17 Q 11 26 20 30 L 20 48 L -20 48 L -20 30 Q -11 26 -11 17 L -11 -17 Q -11 -26 -20 -30 Z" fill="%s"/>' % INK,
            '<circle cx="0" cy="-10" r="3.4" fill="#0a1220"/><circle cx="0" cy="12" r="3.4" fill="#0a1220"/>',
            '<path d="%s" fill="none" stroke="%s" stroke-width="2.6" stroke-linejoin="round" stroke-dasharray="30" stroke-dashoffset="30">'
            '<animate attributeName="stroke-dashoffset" values="30;30;0;0" keyTimes="0;0.35;0.55;1" dur="3s" repeatCount="indefinite"/></path>' % (crack, c),
            '<path d="M -26 -40 L -26 -50 M -30 -45 L -26 -50 L -22 -45 M -26 40 L -26 50 M -30 45 L -26 50 L -22 45" stroke="%s" stroke-width="2" fill="none" '
            'stroke-linecap="round" opacity="0.7"/>' % c]


def g_radar(u, c):
    inc = ''.join('<path d="M %d -26 Q %d 0 %d 26" fill="none" stroke="%s" stroke-width="2.2" stroke-linecap="round"/>' % (x, x + 5, x, c) for x in (-48, -40))
    out = ['<g>%s<animateTransform attributeName="transform" type="translate" values="0 0;12 0" dur="1.4s" repeatCount="indefinite"/>'
           '<animate attributeName="opacity" values="1;0" dur="1.4s" repeatCount="indefinite"/></g>' % inc]
    for k in range(2):
        out.append('<circle cx="-30" cy="0" r="6" fill="none" stroke="%s" stroke-width="1.6" stroke-dasharray="3 5" opacity="0">'
                   '<animate attributeName="r" values="6;40" dur="2.8s" begin="%.1fs" repeatCount="indefinite"/>'
                   '<animate attributeName="opacity" values="0.9;0" dur="2.8s" begin="%.1fs" repeatCount="indefinite"/></circle>' % (c, 1.4 * k, 1.4 * k))
    out.append('<path d="M -32 0 C -22 -9 4 -14 22 -11 C 36 -8 36 8 22 11 C 4 14 -22 9 -32 0 Z" fill="%s"/>' % INK)
    return out


def g_spark(u, c):
    main = 'M 0 -28 L -3 -17 L 2 -8 L -2 3 L 3 13 L -1 23 L 1 33'
    br = 'M 2 -8 L 9 1 L 12 11 M -2 3 L -9 12 L -11 21 M 3 13 L 8 20'
    anim = '<animate attributeName="stroke-dashoffset" values="80;0;0;0" keyTimes="0;0.35;0.8;1" dur="2.4s" repeatCount="indefinite"/>' \
           '<animate attributeName="opacity" values="1;1;1;0" keyTimes="0;0.35;0.8;1" dur="2.4s" repeatCount="indefinite"/>'
    return ['<path d="M -8 -50 L 8 -50 L 8 -44 L 1.2 -28 L -1.2 -28 L -8 -44 Z" fill="%s"/>' % INK,
            '<rect x="-34" y="36" width="68" height="7" rx="2" fill="%s"/>' % INK,
            '<path d="%s %s" fill="none" stroke="%s" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" opacity="0.35"/>' % (main, br, c),
            '<g fill="none" stroke-linecap="round" stroke-linejoin="round" stroke-dasharray="80" stroke-dashoffset="80">'
            '<path d="%s %s" stroke="%s" stroke-width="7" opacity="0.3">%s</path>' % (main, br, c, anim) +
            '<path d="%s %s" stroke="#f1eaff" stroke-width="2.2">%s</path></g>' % (main, br, anim)]


def g_apophis(u, c):
    path = 'M -44 42 Q 66 8 -30 -48'
    return ['<defs><path id="%sp" d="%s"/><radialGradient id="%se" cx="35%%" cy="35%%" r="70%%"><stop offset="0" stop-color="#8fd0ff"/>'
            '<stop offset="1" stop-color="#1f5fa8"/></radialGradient></defs>' % (u, path, u),
            '<circle cx="-6" cy="4" r="25" fill="none" stroke="%s" stroke-width="1.4" stroke-dasharray="2 4" opacity="0.55"/>' % INK,
            '<circle cx="-6" cy="4" r="13" fill="url(#%se)"/>' % u,
            '<path d="%s" fill="none" stroke="%s" stroke-width="1.8" stroke-dasharray="4 4" opacity="0.8"/>' % (path, c),
            '<g><animateMotion dur="5s" repeatCount="indefinite" rotate="0"><mpath href="#%sp"/></animateMotion>' % u +
            '<path d="M -5 -2 Q -4 -6 1 -5 Q 6 -4 5 1 Q 4 6 -1 5 Q -6 4 -5 -2 Z" fill="%s"><animateTransform attributeName="transform" type="rotate" '
            'from="0" to="360" dur="2.2s" repeatCount="indefinite"/></path></g>' % INK]


def g_corona(u, c):
    out = ['<defs><radialGradient id="%sg" cx="50%%" cy="50%%" r="50%%"><stop offset="0.42" stop-color="#ffffff" stop-opacity="0.95"/>'
           '<stop offset="0.6" stop-color="%s" stop-opacity="0.6"/><stop offset="1" stop-color="%s" stop-opacity="0"/></radialGradient>'
           '<linearGradient id="%ss" x1="0.25" y1="0" x2="1" y2="0"><stop offset="0" stop-color="%s" stop-opacity="0.95"/>'
           '<stop offset="1" stop-color="%s" stop-opacity="0"/></linearGradient></defs>' % (u, c, c, u, c, c)]
    petals = []
    for ang, L, w in ((16, 50, 1.0), (165, 48, 0.9), (200, 40, 0.75), (340, 44, 0.85), (122, 30, 0.5), (58, 28, 0.45)):
        petals.append('<path d="M 13 -8 C 24 -8 34 -2 50 0 C 34 2 24 8 13 8 Z" fill="url(#%ss)" transform="rotate(%d) scale(%.2f %.2f)"/>' % (u, ang, L / 50, w))
    for ang in (82, 90, 98, 262, 270, 278):   # the fine rays at the poles
        t = math.radians(ang)
        petals.append('<line x1="%.1f" y1="%.1f" x2="%.1f" y2="%.1f" stroke="%s" stroke-width="1.1" opacity="0.6"/>' % (
            16 * math.cos(t), 16 * math.sin(t), 29 * math.cos(t), 29 * math.sin(t), c))
    out.append('<g>%s<animateTransform attributeName="transform" type="rotate" values="-2;2;-2" dur="14s" repeatCount="indefinite"/></g>' % ''.join(petals))
    out.append('<circle r="27" fill="url(#%sg)"><animate attributeName="opacity" values="0.8;1;0.8" dur="5s" repeatCount="indefinite"/></circle>' % u)
    out.append('<circle r="15.5" fill="#020308"/>')
    out.append('<circle cx="11" cy="-11" r="2" fill="#ffffff" opacity="0"><animate attributeName="opacity" values="0;0;1;0;0" keyTimes="0;0.8;0.86;0.92;1" '
               'dur="6s" repeatCount="indefinite"/><animate attributeName="r" values="1;1;4.5;1;1" keyTimes="0;0.8;0.86;0.92;1" dur="6s" repeatCount="indefinite"/></circle>')
    return out


def g_fusion(u, c):
    out = []
    for k in range(10):   # the field coils round the torus, the back ones first
        a = 2 * math.pi * k / 10 + 0.3
        x, y = 36 * math.cos(a), 12 * math.sin(a)
        if y < 0:
            out.append('<ellipse cx="%.1f" cy="%.1f" rx="%.1f" ry="15" fill="none" stroke="%s" stroke-width="1.5" opacity="0.3"/>' % (x, y, 2.5 + 3 * abs(math.sin(a)), INK))
    out += ['<ellipse cx="0" cy="0" rx="30" ry="10" fill="none" stroke="%s" stroke-width="11" opacity="0.28"><animate attributeName="opacity" values="0.18;0.45;0.18" '
            'dur="2.2s" repeatCount="indefinite"/></ellipse>' % c,
            '<ellipse cx="0" cy="0" rx="30" ry="10" fill="none" stroke="%s" stroke-width="4.5"/>' % c,
            '<ellipse cx="0" cy="0" rx="30" ry="10" fill="none" stroke="#fff3fb" stroke-width="1.8" stroke-dasharray="8 10">'
            '<animate attributeName="stroke-dashoffset" from="0" to="-36" dur="0.9s" repeatCount="indefinite"/></ellipse>']
    for k in range(10):
        a = 2 * math.pi * k / 10 + 0.3
        x, y = 36 * math.cos(a), 12 * math.sin(a)
        if y >= 0:
            out.append('<ellipse cx="%.1f" cy="%.1f" rx="%.1f" ry="15" fill="none" stroke="%s" stroke-width="1.8" opacity="0.85"/>' % (x, y, 2.5 + 3 * abs(math.sin(a)), INK))
    return out


def g_hurricane(u, c):
    arms = []
    for k in range(6):
        a0 = k * math.pi / 3
        inner, outer = [], []
        n = 26
        for i in range(n + 1):
            q = i / n
            r = 9 + 39 * q
            a = a0 - 2.6 * q             # counter-clockwise inwards, as a storm of the northern hemisphere turns
            w = 3.6 * (1 - q) + 0.5
            inner.append(((r - w) * math.cos(a), (r - w) * math.sin(a)))
            outer.append(((r + w) * math.cos(a), (r + w) * math.sin(a)))
        pts = inner + outer[::-1]
        arms.append('<path d="M %s Z" fill="%s" opacity="%.2f"/>' % (' L '.join('%.1f %.1f' % p for p in pts), c if k % 2 == 0 else INK, 0.95 if k % 2 == 0 else 0.7))
    return ['<g>%s<circle r="12" fill="%s" opacity="0.55"/><animateTransform attributeName="transform" type="rotate" from="0" to="-360" dur="18s" '
            'repeatCount="indefinite"/></g>' % (''.join(arms), c),
            '<circle r="4.5" fill="#050a14"/><circle r="4.5" fill="none" stroke="%s" stroke-width="1.4"/>' % INK]


def g_heart(u, c):
    heart = 'M 0 20 C -30 0 -32 -22 -15 -27 C -7 -29 -2 -24 0 -18 C 2 -24 7 -29 15 -27 C 32 -22 30 0 0 20 Z'
    beat = '<animateTransform attributeName="transform" type="scale" values="1;1.09;1;1.05;1;1" keyTimes="0;0.1;0.2;0.3;0.4;1" dur="1.1s" repeatCount="indefinite"/>'
    ecg = 'M -48 24 L -24 24 L -20 20 L -16 24 L -11 24 L -8 29 L -4 -8 L 0 36 L 4 24 L 12 24 L 17 18 L 22 24 L 48 24'
    return ['<g transform="translate(0 -4)"><g>%s<path d="%s" fill="%s"/></g></g>' % (beat, heart, c),
            '<path d="%s" fill="none" stroke="%s" stroke-width="1.6" stroke-linejoin="round" opacity="0.35"/>' % (ecg, INK),
            '<path d="%s" fill="none" stroke="%s" stroke-width="2.6" stroke-linejoin="round" stroke-linecap="round" stroke-dasharray="190" stroke-dashoffset="190">'
            '<animate attributeName="stroke-dashoffset" from="190" to="-190" dur="2.2s" repeatCount="indefinite"/></path>' % (ecg, INK)]


def g_shedding(u, c):
    street = ''.join('<path d="%s" fill="none" stroke="%s" stroke-width="2.4" stroke-linecap="round"/>' % (_spiral(x, y, 6, 0.8, s), c)
                     for x, y, s in ((-8, -7, -1), (8, 7, 1), (24, -7, -1), (40, 7, 1)))
    return _flowlines((-30, 30), -50, 46, c, op=0.4) + ['<circle cx="-28" cy="0" r="11" fill="%s"/>' % INK, _drift(street, 16, 2.6, 0), _drift(street, 16, 2.6, 1.3)]


def g_bowshock(u, c):
    return _flowlines((-34, -14, 14, 34), -50, -30, INK, 0.8, 0.5) + [
        '<path d="M 30 -46 Q -46 0 30 46" fill="none" stroke="%s" stroke-width="3"><animate attributeName="opacity" values="0.7;1;0.7" dur="1.6s" repeatCount="indefinite"/></path>' % c,
        '<circle cx="14" cy="0" r="12" fill="%s"/>' % INK]


def g_bubble(u, c):
    return ['<ellipse cx="6" cy="0" rx="15" ry="15" fill="%s" fill-opacity="0.25" stroke="%s" stroke-width="2.4">'
            '<animate attributeName="rx" values="15;15;9;15" keyTimes="0;0.4;0.85;1" dur="3.2s" repeatCount="indefinite"/>'
            '<animate attributeName="ry" values="15;15;19;15" keyTimes="0;0.4;0.85;1" dur="3.2s" repeatCount="indefinite"/></ellipse>' % (c, c),
            '<line x1="-44" y1="-40" x2="-44" y2="40" stroke="%s" stroke-width="3.2" stroke-linecap="round">'
            '<animate attributeName="x1" values="-44;46" dur="3.2s" repeatCount="indefinite"/><animate attributeName="x2" values="-44;46" dur="3.2s" repeatCount="indefinite"/>'
            '</line>' % INK]


def g_planets(u, c):
    out = ['<circle r="7" fill="#ffd166"/><circle r="11" fill="#ffd166" opacity="0.25"/>']
    for k, (rx, ry, d, r) in enumerate(((20, 8, 3.0, 2.6), (32, 13, 6.0, 3.2), (46, 19, 11.0, 3.6))):
        p = 'M %d 0 A %d %d 0 1 1 %d 0 A %d %d 0 1 1 %d 0' % (rx, rx, ry, -rx, rx, ry, rx)
        out.append('<path id="%so%d" d="%s" fill="none" stroke="%s" stroke-width="1.3" opacity="0.45"/>' % (u, k, p, INK))
        out.append('<circle r="%.1f" fill="%s"><animateMotion dur="%.1fs" repeatCount="indefinite"><mpath href="#%so%d"/></animateMotion></circle>' % (r, c if k != 1 else INK, d, u, k))
    return out


GLYPHS = {'car-wake': g_car, 'reentry-fire-ii': g_reentry, 'jet-flame': g_flame, 'drop-splash': g_splash, 'metal-printing': g_print,
          'metal-fracture': g_fracture, 'radar-almond': g_radar, 'spark-streamer': g_spark, 'apophis-2029': g_apophis,
          'corona-2027': g_corona, 'fusion-shot': g_fusion, 'hurricane-otis': g_hurricane, 'heartbeat': g_heart,
          'flow-cylinder-shedding': g_shedding, 'gas-sphere-bowshock': g_bowshock, 'gas-shock-bubble': g_bubble,
          'orbit-planets-2020': g_planets}


def glyph(fid, x, y, scale, u):
    fn = GLYPHS.get(fid)
    if not fn:
        return []
    return ['<g transform="translate(%.1f %.1f) scale(%.3f)">' % (x, y, scale)] + fn(u, STONE[fid]) + ['</g>']


def svg_open(W, H, title):
    return ['<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" viewBox="0 0 %d %d" font-family="%s" role="img">' % (W, H, W, H, FONT),
            '<title>%s</title>' % esc(title)]


def starfield(rng, W, H, n, twinkle=0):
    out = []
    for i in range(n):
        x, y, r = rng.uniform(0, W), rng.uniform(0, H), rng.choice([0.5, 0.7, 0.9, 1.2])
        o = rng.uniform(0.15, 0.7)
        if i < twinkle:
            d = rng.uniform(2.5, 6.0)
            out.append('<circle cx="%.1f" cy="%.1f" r="%.1f" fill="#dfe8ff" opacity="%.2f"><animate attributeName="opacity" values="%.2f;0.05;%.2f" '
                       'dur="%.1fs" begin="%.1fs" repeatCount="indefinite"/></circle>' % (x, y, r, o, o, o, d, rng.uniform(0, 5)))
        else:
            out.append('<circle cx="%.1f" cy="%.1f" r="%.1f" fill="#dfe8ff" opacity="%.2f"/>' % (x, y, r, o))
    return out


def text_width(s, size):
    return 0.56 * size * len(s)


def fit(s, size, width):
    return min(size, width / max(1e-9, 0.56 * len(s)))


DISPLAY = "'SF Pro Display', -apple-system, BlinkMacSystemFont, Inter, 'Segoe UI', 'Helvetica Neue', Arial, sans-serif"


def svg_thirteen(mains, attempts):
    """The front page's picture: the thirteen as gems in two rows, the grails larger, and nothing else but a title and
    its call, in grey: capture one, or how many are still free."""
    W, H = 1280, 700
    p = svg_open(W, H, 'The thirteen flags')
    p.append('<defs>')
    p.append('<radialGradient id="lift" cx="50%" cy="42%" r="70%"><stop offset="0" stop-color="#11151d"/><stop offset="1" stop-color="#000000"/></radialGradient>')
    p += defs_gems()
    p.append('</defs>')
    p.append('<rect width="%d" height="%d" rx="28" fill="url(#lift)"/>' % (W, H))
    free = sum(1 for f in mains if not any(a['flag'] == f['id'] and a['score'] >= CAPTURE and not a.get('baseline') for a in attempts))
    words = ['None', 'One', 'Two', 'Three', 'Four', 'Five', 'Six', 'Seven', 'Eight', 'Nine', 'Ten', 'Eleven', 'Twelve', 'Thirteen']
    call = 'Capture one.' if free == len(mains) else '%s still free.' % words[free] if free else 'All captured.'
    p.append('<text x="640" y="104" text-anchor="middle" fill="#f5f5f7" font-family="%s" font-size="60" font-weight="700" '
             'letter-spacing="-1.8">Thirteen flags. <tspan fill="#86868b">%s</tspan></text>' % (DISPLAY, call))
    by_tier = {k: [f for f in mains if f['tier'] == k] for k in TIERS}
    for i, f in enumerate(by_tier['flag']):
        p += medallion(136 + 144 * i, 250, 58, f, flag_stats(f['id'], attempts), label_y=250 + 58 + 38)
    groups = [('semi-holy-grail', 'Semi holy grails', [250, 450], 66), ('holy-grail', 'Holy grails', [700, 910, 1120], 74)]
    for tier, name, xs, r in groups:
        p.append('<text x="%.0f" y="438" text-anchor="middle" fill="%s" font-family="%s" font-size="17" font-weight="600">%s</text>'
                 % (sum(xs) / len(xs), TIERS[tier]['accent'], DISPLAY, name))
        for x, f in zip(xs, by_tier[tier]):
            p += medallion(x, 548, r, f, flag_stats(f['id'], attempts), label_y=548 + 74 + 38)
    p.append('</svg>')
    return '\n'.join(p) + '\n'


def medallion(cx, cy, r, f, st, label=True, label_y=None):
    """A flag as a gem: a dark core with a fine rim in its tier's colour and its glyph inside; the holy grails glow. Needs
    the document's defs to hold core-<tier> and blur (defs_gems)."""
    t = TIERS[f['tier']]
    out = []
    if f['tier'] == 'holy-grail':
        out.append('<circle cx="%d" cy="%d" r="%d" fill="%s" filter="url(#blur)" opacity="0.28"/>' % (cx, cy, r + 8, t['accent']))
    out.append('<circle cx="%d" cy="%d" r="%d" fill="url(#core-%s)"/>' % (cx, cy, r, f['tier']))
    out.append('<circle cx="%d" cy="%d" r="%.1f" fill="none" stroke="%s" stroke-width="%.1f" stroke-opacity="%.2f"/>' % (
        cx, cy, r - 0.75, t['accent'], 3.0 if st['first'] else 1.5, 1.0 if st['first'] else 0.75))
    out += glyph(f['id'], cx, cy, r / 64.0, 'g-%s-' % f['id'])
    if label:
        ly = label_y or cy + r + 38
        num = '%02d' % f['number'] if f.get('number') else f.get('label', '')
        out.append('<text x="%d" y="%d" text-anchor="middle" font-family="%s" font-size="18" font-weight="600" fill="#f5f5f7">'
                   '<tspan fill="#86868b" font-weight="500">%s</tspan>  %s</text>' % (cx, ly, DISPLAY, num, esc(f['short'])))
        stone = f.get('stone_short', f['stone'])
        out.append('<text x="%d" y="%d" text-anchor="middle" font-family="%s" font-size="14" fill="#86868b">%s</text>' % (
            cx, ly + 22, DISPLAY, esc(stone[0].upper() + stone[1:])))
    return out


def defs_gems():
    out = ['<radialGradient id="core-%s" cx="35%%" cy="30%%" r="75%%"><stop offset="0" stop-color="%s"/><stop offset="1" stop-color="%s"/></radialGradient>'
           % (k, t['core0'], t['core1']) for k, t in TIERS.items()]
    out.append('<filter id="blur" x="-50%" y="-50%" width="200%" height="200%"><feGaussianBlur stdDeviation="9"/></filter>')
    return out


def svg_card(f, st):
    W, H = 1280, 240
    t = TIERS[f['tier']]
    rng = random.Random(f['id'])
    p = svg_open(W, H, '%s: %s' % (f.get('number') and 'Flag %02d' % f['number'] or 'Trial', f['title']))
    p.append('<defs>')
    p.append('<linearGradient id="bg" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="%s"/><stop offset="1" stop-color="%s"/></linearGradient>' % (t['bg0'], t['bg1']))
    p.append('<linearGradient id="shine" x1="0" y1="0" x2="1" y2="0"><stop offset="0" stop-color="#ffffff" stop-opacity="0"/>'
             '<stop offset="0.5" stop-color="#ffffff" stop-opacity="0.10"/><stop offset="1" stop-color="#ffffff" stop-opacity="0"/></linearGradient>')
    p.append('<clipPath id="card"><rect x="1" y="1" width="%d" height="%d" rx="22"/></clipPath>' % (W - 2, H - 2))
    p += defs_gems()
    p.append('</defs>')
    p.append('<g clip-path="url(#card)"><rect width="%d" height="%d" fill="url(#bg)"/>' % (W, H))
    if f['tier'] in ('semi-holy-grail', 'holy-grail'):
        p += starfield(rng, W, H, 60, twinkle=10)
    if f['tier'] == 'holy-grail':
        p.append('<rect x="-420" y="0" width="360" height="%d" fill="url(#shine)" transform="skewX(-18)"><animate attributeName="x" from="-420" to="1500" '
                 'dur="7s" repeatCount="indefinite"/></rect>' % H)
    p.append('</g>')
    p.append('<rect x="1" y="1" width="%d" height="%d" rx="22" fill="none" stroke="%s" stroke-opacity="0.4" stroke-width="1.5"/>' % (W - 2, H - 2, t['accent']))
    p += medallion(128, 120, 80, f, st, label=False)
    x0 = 262
    chips = [('%s %s' % (t['label'], '%02d' % f['number'] if f.get('number') else f.get('label', '')), t['accent'])]
    if f.get('stone'):
        chips.append(('STONE · %s' % f['stone'].upper(), '#b9c4d6'))
    x = x0
    for text, col in chips:
        w = 11.2 * len(text) * 0.78 + 30
        p.append('<rect x="%d" y="30" width="%.0f" height="28" rx="14" fill="%s" fill-opacity="0.12" stroke="%s" stroke-opacity="0.55"/>' % (x, w, col, col))
        p.append('<text x="%.0f" y="49" text-anchor="middle" fill="%s" font-size="12" font-weight="700" letter-spacing="2.2">%s</text>' % (x + w / 2, col, esc(text)))
        x += w + 12
    size = fit(f['title'], 40, 600)
    p.append('<text x="%d" y="112" fill="#f4f7fb" font-size="%.1f" font-weight="700">%s</text>' % (x0, size, esc(f['title'])))
    size = fit(f['subtitle'], 19, 600)
    p.append('<text x="%d" y="146" fill="#a9b5c8" font-size="%.1f">%s</text>' % (x0, size, esc(f['subtitle'])))
    key, pill, _ = status_of(f, st)
    col = {'captured': '#f2c14e', 'open': '#6fdc8c', 'awaiting': '#dfe7f2', 'preparing': '#8fa3c0'}[key]
    w = 9.2 * len(pill) * 0.86 + 34
    p.append('<rect x="%d" y="176" width="%.0f" height="30" rx="15" fill="%s" fill-opacity="0.14" stroke="%s" stroke-opacity="0.7"/>' % (x0, w, col, col))
    p.append('<text x="%.0f" y="196" text-anchor="middle" fill="%s" font-size="13" font-weight="700" letter-spacing="1.8">%s</text>' % (x0 + w / 2, col, esc(pill)))
    if f.get('deadline'):
        p.append('<text x="%.0f" y="196" fill="#c6d0df" font-size="14">Predictions close %s · %s</text>' % (x0 + w + 16, esc(f['deadline']), esc(f.get('event_words', ''))))
    # the stats panel
    p.append('<rect x="910" y="22" width="346" height="196" rx="16" fill="#000000" fill-opacity="0.26" stroke="#ffffff" stroke-opacity="0.06"/>')
    rec = st['record']
    rows = [('DIFFICULTY', difficulty(f)), ('ATTEMPTS', str(st['attempts'])), ('CHALLENGERS', str(st['challengers'])),
            ('RECORD', '%d · @%s' % (rec['score'], rec['who']) if rec else 'unclaimed')]
    if not f.get('difficulty'):
        rows = rows[1:]
    for i, (k, v) in enumerate(rows):
        y = 62 + i * 44
        p.append('<text x="934" y="%d" fill="#7e8aa0" font-size="11.5" font-weight="700" letter-spacing="2.4">%s</text>' % (y, k))
        vs = 26 if k == 'DIFFICULTY' and v == '∞' else 19
        colour = t['accent'] if k in ('DIFFICULTY', 'RECORD') and rec or k == 'DIFFICULTY' else '#e9eef6'
        p.append('<text x="1232" y="%d" text-anchor="end" fill="%s" font-size="%d" font-weight="700">%s</text>' % (y + 2, colour, fit(v, vs, 190), esc(v)))
    p.append('</svg>')
    return '\n'.join(p) + '\n'


def svg_podium(mains, attempts):
    W, H = 1280, 400
    rng = random.Random(3)
    h = hall(mains, attempts)
    p = svg_open(W, H, 'Hall of fame')
    p.append('<defs><linearGradient id="bg" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#04070e"/><stop offset="1" stop-color="#0d1629"/></linearGradient>')
    for k, (a, b) in {'g1': ('#f7d88a', '#b8860b'), 'g2': ('#e4e8ee', '#8e98a6'), 'g3': ('#e6b58a', '#8b5a2b')}.items():
        p.append('<linearGradient id="%s" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="%s"/><stop offset="1" stop-color="%s"/></linearGradient>' % (k, a, b))
    p.append('</defs><rect width="%d" height="%d" fill="url(#bg)"/>' % (W, H))
    p += starfield(rng, W, H, 90, twinkle=14)
    p.append('<text x="640" y="54" text-anchor="middle" fill="#eef3fb" font-size="28" font-weight="700" letter-spacing="8">HALL OF FAME</text>')
    p.append('<text x="640" y="82" text-anchor="middle" fill="#8b97ab" font-size="14">Flags captured first, then power: a flag counts once, a semi holy grail twice, a holy grail three times</text>')
    slots = [(1, 515, 190, 'g1'), (2, 250, 238, 'g2'), (3, 780, 270, 'g3')]  # rank, x, top, gradient
    for rank, x, top, g in slots:
        wdt = 250
        p.append('<rect x="%d" y="%d" width="%d" height="%d" rx="8" fill="url(#%s)"/>' % (x, top, wdt, H - 22 - top, g))
        p.append('<text x="%d" y="%d" text-anchor="middle" fill="#10151f" fill-opacity="0.75" font-size="46" font-weight="800">%d</text>' % (x + wdt / 2, top + 58, rank))
        if rank <= len(h):
            e = h[rank - 1]
            p.append('<text x="%d" y="%d" text-anchor="middle" fill="#f4f7fb" font-size="%.1f" font-weight="700">@%s</text>' % (x + wdt / 2, top - 40, fit('@' + e['who'], 22, 240), esc(e['who'])))
            p.append('<text x="%d" y="%d" text-anchor="middle" fill="#f2c14e" font-size="15">%d captured · power %d</text>' % (x + wdt / 2, top - 16, e['captured'], e['power']))
        else:
            p.append('<text x="%d" y="%d" text-anchor="middle" fill="#6c7890" font-size="20" font-style="italic">unclaimed</text>' % (x + wdt / 2, top - 38))
            p.append('<text x="%d" y="%d" text-anchor="middle" fill="#4c5870" font-size="14">your name here<animate attributeName="opacity" '
                     'values="0.35;1;0.35" dur="3.5s" begin="%ds" repeatCount="indefinite"/></text>' % (x + wdt / 2, top - 14, rank))
    p.append('</svg>')
    return '\n'.join(p) + '\n'


# ---------------------------------------------------------------------------------------------------- writing it all

def leaderboard_md(mains, warms, attempts, scored_on):
    L = ['# Leaderboard', '',
         'Up: [the front page](../README.md) · how the flags work: [README.md](README.md) · generated by `tools/flags.py board` from '
         '[attempts.jsonl](attempts.jsonl) and each flag\'s own files; last scored on %s.' % scored_on, '',
         stats_line(mains, warms, attempts), '', '## Hall of fame', '', hall_md(mains, attempts, rel='../'), '', '## The thirteen flags', '']
    for f in mains:
        st = flag_stats(f['id'], attempts)
        L += ['### Flag %02d · [%s](%s/FLAG.md)' % (f['number'], f['title'], f['id']), '',
              '%s · stone: %s · difficulty %s · %s' % (TIERS[f['tier']]['name'], f['stone'], difficulty(f), status_of(f, st)[2]), '',
              board_md(f, st, rel='../'), '']
    L += ['## Trials', '', 'Smaller challenges, each leading to one of the thirteen. They were scored with the lab\'s two-dimensional solvers and are being '
          'rebuilt natively in 3D ([GOALS.md](../GOALS.md) G15).', '', warmups_md(warms, mains, attempts, rel='../'), '']
    for f in warms:
        L += ['### [%s](%s/FLAG.md)' % (f['title'], f['id']), '', board_md(f, flag_stats(f['id'], attempts), rel='../'), '']
    L += ['## How a score is formed', '',
          'Each case is scored against its hidden measurement: exp(-e^2 / 2), with e the error in units of the measurement\'s '
          'uncertainty (never below %d per cent), so a prediction within the scatter of the experiment scores near 1. A flag\'s score '
          'is 100 times the mean over its cases and outputs, published in bands of %d points. %d captures a flag. The details, and why '
          'the score cannot be inverted into the answers: [README.md](README.md#how-a-score-is-formed).' % (int(100 * SCORE_FLOOR), BAND, CAPTURE), '']
    return '\n'.join(L)


def drawn_files():
    """Everything `board` draws, as {path: text}, from the flags' files and the attempts; nothing is written here."""
    flags = load_flags()
    mains, warms = main_flags(flags), warmups(flags)
    attempts = load_attempts()
    s = json.loads((FLAGS / 'scores.json').read_text()) if (FLAGS / 'scores.json').exists() else {'scored_on': 'never'}
    out = {FLAGS / 'thirteen.svg': svg_thirteen(mains, attempts), FLAGS / 'podium.svg': svg_podium(mains, attempts)}
    for f in mains + warms:
        st = flag_stats(f['id'], attempts)
        out[f['_dir'] / 'card.svg'] = svg_card(f, st)
        page = f['_dir'] / 'FLAG.md'
        if page.exists():
            text, ok = replace_block(page.read_text(), 'board', board_md(f, st, rel='../../'))
            if ok:
                out[page] = text
    out[FLAGS / 'LEADERBOARD.md'] = leaderboard_md(mains, warms, attempts, s.get('scored_on', 'never'))
    readme = ROOT / 'README.md'
    text = readme.read_text()
    text, _ = replace_block(text, 'stats', stats_line(mains, warms, attempts))
    text, _ = replace_block(text, 'summary', summary_md(mains, attempts))
    text, _ = replace_block(text, 'hall', hall_md(mains, attempts))
    text, _ = replace_block(text, 'trials', warmups_md(warms, mains, attempts))
    for f in mains + warms:
        text, _ = replace_block(text, 'board:%s' % f['id'], board_md(f, flag_stats(f['id'], attempts), top=3))
    out[readme] = text
    return out, len(mains) + len(warms)


def cmd_board(args):
    out, n = drawn_files()
    stale = [p for p, text in out.items() if not p.exists() or p.read_text() != text]
    if args.check:
        for p in stale:
            print('out of date: %s' % p.relative_to(ROOT))
        if stale:
            sys.exit('%d drawn file(s) differ from what the attempts and the flags\' files give; run python3 tools/flags.py board' % len(stale))
        print('the boards, the cards and the front page agree with the attempts and the flags\' files (%d files)' % len(out))
        return
    for p in stale:
        p.write_text(out[p])
    print('drew flags/thirteen.svg, flags/podium.svg, %d cards, flags/LEADERBOARD.md, the flag pages\' boards and the front page\'s tables '
          '(%d of %d files changed)' % (n, len(stale), len(out)))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    a = sub.add_parser('seal')
    a.add_argument('draft')
    a.add_argument('--remove-draft', action='store_true')
    for n in ('run', 'verify'):
        a = sub.add_parser(n)
        a.add_argument('--entry', required=True)
        if n == 'run':
            a.add_argument('--flag', help='run only this flag and merge into the stored predictions')
    sub.add_parser('score')
    a = sub.add_parser('board')
    a.add_argument('--check', action='store_true', help='draw nothing; fail if a drawn file is out of date (for CI)')
    sub.add_parser('check-commitments')
    a = sub.add_parser('judge', help='CI: score one pull request\'s rerun (see .github/workflows/flag-score.yml)')
    a.add_argument('--predictions', required=True)
    a.add_argument('--entry-json', required=True)
    a.add_argument('--author', required=True, help='the GitHub login that opened the pull request')
    a.add_argument('--sha', required=True, help='the pull request\'s head commit')
    a.add_argument('--files', help='a file listing the paths the pull request changes')
    a.add_argument('--comment', default='judge-comment.md')
    a.add_argument('--label', default='judge-label.txt')
    sub.add_parser('export-sealed', help='maintainer: print the sealed store as one JSON object, for the CI secret')
    args = ap.parse_args()
    {'seal': cmd_seal, 'run': cmd_run, 'verify': cmd_verify, 'score': cmd_score, 'board': cmd_board,
     'check-commitments': cmd_check, 'judge': cmd_judge, 'export-sealed': cmd_export}[args.cmd](args)


if __name__ == '__main__':
    main()
