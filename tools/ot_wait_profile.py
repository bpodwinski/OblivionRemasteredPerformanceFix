#!/usr/bin/env python3
"""Compare sampled stacks in low/high old-engine wait frames, not just FPS hitches."""
import argparse
import bisect
import collections
import csv
import json
import math
import pathlib
import re
import statistics
import sys


def load_frames(path, lo, hi):
    columns, meta, selected, sessions = None, None, [], set()
    required = {'frame', 'end_ms', 'period_ms', 'sleep_ms', 'pair_ms', 'tg3_ms', 'grid_ms', 'state'}
    with path.open(encoding='utf-8-sig') as f:
        for line in f:
            if line.startswith('# OblivionPerfFix-ledger'):
                meta = line.strip()
                columns = None
            elif line.startswith('#F,'):
                columns = line.strip()[1:].split(',')
                if not required.issubset(columns):
                    raise ValueError('unsupported ledger frame columns')
            elif line.startswith('F,') and columns:
                values = line.strip().split(',')
                if len(values) != len(columns):
                    raise ValueError('malformed frame row')
                row = dict(zip(columns, values))
                end, period, sleep, pair, phys, grid = [float(row[k]) for k in
                    ('end_ms', 'period_ms', 'sleep_ms', 'pair_ms', 'tg3_ms', 'grid_ms')]
                if not all(math.isfinite(v) for v in (end, period, sleep, pair, phys, grid)):
                    raise ValueError('non-finite ledger value')
                if row['state'] != 'play' or end - period < lo or end > hi:
                    continue
                if not meta or not re.search(r'threshold_ms=5(?:\.0+)?(?:,|$)', meta):
                    raise ValueError('capture with HitchLedgerMs=5 from startup; other thresholds bias cohorts')
                if not re.search(r'(?:,|:)pacing:1(?:,|$)', meta):
                    raise ValueError('ledger lacks the pairing signal (NpcCreationPacing hook)')
                if period <= 0 or not 0 <= sleep <= period or min(pair, phys, grid) < 0:
                    raise ValueError('invalid ledger duration')
                sessions.add(meta)
                selected.append((end - period, end - sleep, end, pair, phys, grid, int(row['frame'])))
    if len(sessions) != 1 or not selected:
        raise ValueError('expected one ledger session with complete play frames in the scene')
    selected.sort()
    for a, b in zip(selected, selected[1:]):
        if b[6] <= a[6] or b[0] < a[2] - 0.005:
            raise ValueError('overlapping frames or mixed ledger sessions')
    return selected


def load_samples(prefix):
    stacks = pathlib.Path(str(prefix) + '-stacks.txt').read_text(encoding='utf-8-sig').splitlines()
    samples = collections.defaultdict(list)
    with pathlib.Path(str(prefix) + '-timeline.csv').open(encoding='utf-8-sig', newline='') as f:
        reader = csv.DictReader(f)
        if not {'QpcMs', 'Thread', 'Stack'}.issubset(reader.fieldnames or []):
            raise ValueError('unsupported stack timeline columns')
        for row in reader:
            t, idx = float(row['QpcMs']), int(row['Stack'])
            if not math.isfinite(t) or not 0 <= idx < len(stacks):
                raise ValueError('invalid stack timeline row')
            samples[row['Thread']].append((t, stacks[idx]))
    for rows in samples.values():
        rows.sort()
    return samples


def compare(frames, samples, low, high, depth):
    groups = {name: [] for name in ('low', 'middle', 'high')}
    for frame in frames:
        pair = frame[3]
        groups['low' if pair <= low else 'high' if pair >= high else 'middle'].append(frame)
    report = {}
    for name, group in groups.items():
        result = {'frames': len(group), 'threads': {}}
        if group:
            for key, fn in [('game_ms', lambda f: f[1]-f[0]), ('pair_ms', lambda f: f[3]),
                            ('physics_ms', lambda f: f[4]), ('grid_ms', lambda f: f[5])]:
                result[key] = statistics.median(map(fn, group))
        for thread, rows in samples.items():
            times = [r[0] for r in rows]
            counts, covered = collections.Counter(), 0
            for start, work_end, *_ in group:
                i, j = bisect.bisect_left(times, start), bisect.bisect_left(times, work_end)
                covered += int(i < j)
                for _, stack in rows[i:j]:
                    inner = ';'.join(stack.split(';')[1:][-depth:]) or '<empty>'
                    inner = inner.replace('OblivionRemastered-Win64-Shipping.exe+', 'exe+')
                    counts[inner] += 1
            result['threads'][thread] = {'covered': covered, 'samples': sum(counts.values()), 'stacks': counts}
        report[name] = result
    return report


def render(report, frames, low, high, top):
    out = ['# Old-engine wait profile', '',
           'Whole work windows (cap sleep excluded); sample shares are not CPU time or proof of causation.',
           'Only complete recorded play frames; no imputation for missing samples or frames.', '',
           f'Low <= {low:g} ms; high >= {high:g} ms of pair wait.', '',
           '| Cohort | Frames | Game ms median | Pair ms median | Physics wait median | Grid ms median |',
           '|---|---:|---:|---:|---:|---:|']
    for name, g in report.items():
        vals = [f'{g[k]:.3f}' if k in g else '-' for k in ('game_ms','pair_ms','physics_ms','grid_ms')]
        out.append('| ' + ' | '.join([name, str(g['frames']), *vals]) + ' |')
    gaps = sum(max(0, b[6]-a[6]-1) for a,b in zip(frames, frames[1:]))
    out += ['', f'Frame-number gaps between selected frames: {gaps} (may include omitted non-play or <5 ms frames).']
    if min(report['low']['frames'], report['high']['frames']) < 20:
        out += ['', '**Insufficient low/high frames for a strong contrast.** Capture another state; no inferred gain.']
    for thread in sorted(report['low']['threads']):
        a, b = (report[g]['threads'][thread] for g in ('low','high'))
        out += ['', f'## {thread}', '', f'Low: {a["samples"]} samples in {a["covered"]}/{report["low"]["frames"]} frames; '
                f'high: {b["samples"]} samples in {b["covered"]}/{report["high"]["frames"]} frames.']
        if not a['samples'] or not b['samples']:
            out += ['', 'One cohort has no samples; shares and deltas are not comparable.']
        else:
            out += ['', '| Stack (root to leaf suffix) | Low % | High % | Delta pp |', '|---|---:|---:|---:|']
        rows = []
        for stack in a['stacks'].keys() | b['stacks'].keys():
            x = 100*a['stacks'][stack]/a['samples'] if a['samples'] else None
            y = 100*b['stacks'][stack]/b['samples'] if b['samples'] else None
            delta = y-x if x is not None and y is not None else None
            rows.append((stack,x,y,delta))
        if a['samples'] and b['samples']:
            for stack,x,y,d in sorted(rows,key=lambda r:(-abs(r[3]),r[0]))[:top]:
                out.append(f'| {stack.replace("|", "/")} | {x:.1f} | {y:.1f} | {d:+.1f} |')
        else:
            for label, cohort in [('low',a),('high',b)]:
                for stack,n in cohort['stacks'].most_common(top):
                    out.append(f'- {label}: {n} samples: {stack}')
    return '\n'.join(out)+'\n'


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--run', type=pathlib.Path, required=True)
    p.add_argument('--scene', required=True)
    p.add_argument('--ledger', type=pathlib.Path, required=True)
    p.add_argument('--low', type=float, default=0.5)
    p.add_argument('--high', type=float, default=2.0)
    p.add_argument('--depth', type=int, default=8)
    p.add_argument('--top', type=int, default=20)
    p.add_argument('--out', type=pathlib.Path)
    a = p.parse_args()
    try:
        if not (0 <= a.low < a.high < math.inf and a.depth > 0 and a.top > 0):
            raise ValueError('require 0 <= low < high and positive depth/top')
        r = json.loads((a.run/'result.json').read_text(encoding='utf-8-sig'))
        scenes = [s for session in r['sessions'] for s in session.get('scenes',[]) or [] if s['id']==a.scene]
        if len(scenes)!=1:
            raise ValueError('scene must occur exactly once in result.json')
        lo,hi = (float(scenes[0][k]) for k in ('t0Ms','t1Ms'))
        if not math.isfinite(lo) or not math.isfinite(hi) or lo>=hi:
            raise ValueError('invalid scene QPC interval')
        frames = load_frames(a.ledger,lo,hi)
        samples = load_samples(a.run/'stacks'/re.sub(r'[\\/]','-',a.scene))
        if not {'GameThread','OblivionThread'}.issubset(samples):
            raise ValueError('sample both GameThread and OblivionThread')
        report = compare(frames,samples,a.low,a.high,a.depth)
        if not any(g['threads']['OblivionThread']['samples'] for g in report.values()):
            raise ValueError('no OblivionThread samples overlap selected frame work windows')
        md = render(report,frames,a.low,a.high,a.top)
        if a.out:
            a.out.write_text(md,encoding='utf-8')
        else:
            print(md,end='')
    except (OSError, ValueError, KeyError, TypeError) as e:
        p.exit(2, f'error: {e}\n')


if __name__ == '__main__':
    main()
