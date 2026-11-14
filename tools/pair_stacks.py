#!/usr/bin/env python3
"""Where the old-engine wait ("pair") goes: the game thread and the OblivionThread side by side, from a stack timeline.

Input: a benchmark run folder made with -Stacks GameThread,OblivionThread (-StackSampler fast -StackIntervalMs 1 is
best); its stacks/<scene>-timeline.csv and -stacks.txt, and result.json for the scene's measured interval.

Each sweep samples both threads within microseconds. Per sweep the game thread is classed as
  pair-wait   inside the pre-actor-tick RunSession(-1), parked in the session step's semaphore wait (the OT is late);
  pair-run    inside that RunSession, running a command the OT queued (GT-side work done for the old engine);
  other       anything else (tick groups, render sync, ...).
The OblivionThread is classed as waiting (its own RunSession step parked: it waits for the GT) or working. Output:
the shares, then the OT's functions (inclusive and leaf) while the GT is in pair-wait: that is the work the GT waits for.
Function names come from the "Name (exe+0x...)" comments in src/ when one sits at that function's start.

    py -3 tools/pair_stacks.py captures/bench/<stamp>-pair-S1 [--scene actors-battle] [--top 40]
"""
import argparse
import bisect
import csv
import json
import os
import re
import sys
from collections import Counter

EXE = "OblivionRemastered-Win64-Shipping.exe"
RUN_SESSION = 0x4591010      # pairing manager RunSession(key)
SEM_LOCK = 0xF5D550          # the session step's semaphore Lock
PRE_TICK, PRE_TICK_END = 0x4899B50, 0x4899F00   # OnWorldPreActorTick handler (the per-frame RunSession(-1) at +0x2B9)
WAIT_MODULES = ("ntdll.dll", "KERNELBASE.dll", "KERNEL32.DLL", "win32u.dll")


def parse_frame(f):
    mod, _, off = f.partition("+0x")
    try:
        return mod, int(off, 16)
    except ValueError:
        return mod, -1


def load_names(root):
    names = {}
    pat = re.compile(r"([A-Za-z_~][\w:<>~]*(?:::[\w~]+)*)\s*(?:'s [\w ]+)?\s*\((?:[^()]{0,40}?)exe\+0x([0-9A-Fa-f]+)")
    for d, _, files in os.walk(os.path.join(root, "src")):
        for fn in files:
            if not fn.endswith((".cpp", ".h")):
                continue
            try:
                text = open(os.path.join(d, fn), encoding="utf-8", errors="replace").read()
            except OSError:
                continue
            for m in pat.finditer(text):
                rva = int(m.group(2), 16)
                names.setdefault(rva, m.group(1))
    return names


def label(frame, names):
    mod, off = parse_frame(frame)
    if mod == EXE:
        n = names.get(off)
        return f"exe+0x{off:X}" + (f" {n}" if n else "")
    return frame.replace(".dll", "").replace(".DLL", "")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("run")
    ap.add_argument("--scene")
    ap.add_argument("--top", type=int, default=35)
    a = ap.parse_args()
    repo = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    names = load_names(repo)

    sdir = os.path.join(a.run, "stacks")
    scenes = sorted(f[:-len("-timeline.csv")] for f in os.listdir(sdir) if f.endswith("-timeline.csv"))
    scene = a.scene or scenes[0]
    t0 = t1 = None
    try:
        res = json.load(open(os.path.join(a.run, "result.json"), encoding="utf-8-sig"))
        for sess in res.get("sessions", []):
            for seg in sess.get("scenes", []):
                if seg.get("id") == scene and seg.get("t0Ms"):
                    t0, t1 = float(seg["t0Ms"]), float(seg["t1Ms"])
    except (OSError, ValueError, KeyError):
        pass
    stacks = [l.rstrip("\n").split(";") for l in open(os.path.join(sdir, scene + "-stacks.txt"), encoding="utf-8")]
    rows = []
    with open(os.path.join(sdir, scene + "-timeline.csv"), encoding="utf-8") as fh:
        for r in csv.DictReader(fh):
            t = float(r["QpcMs"])
            if t0 is not None and not (t0 <= t <= t1):
                continue
            rows.append((t, r["Thread"], int(r["Stack"])))
    rows.sort()
    gt = [(t, s) for t, th, s in rows if th == "GameThread"]
    ot = [(t, s) for t, th, s in rows if th == "OblivionThread"]
    if not gt or not ot:
        sys.exit("no GameThread / OblivionThread samples in the measured interval")

    def frames(i):
        return stacks[i][1:]  # root first, the thread name dropped

    # A frame is the function's start when the unwinder found its .pdata entry, else the return address: match the
    # pre-tick handler by range. Below RunSession: the step (exe+0x4590F80, its semaphore Lock exe+0xF5D550 = waiting)
    # or the replay of the read buffer (exe+0x4590FF0, whose callee is the command's function).
    def below_run_session(fs):
        idx = [j for j, (m, o) in enumerate(fs) if m == EXE and o == RUN_SESSION]
        return fs[idx[-1] + 1:] if idx else None

    def is_wait(below):
        return (not below or any(m == EXE and o == SEM_LOCK for m, o in below)
                or all(m in WAIT_MODULES for m, _ in below))

    def gt_class(i):
        fs = [parse_frame(f) for f in frames(i)]
        if not any(m == EXE and PRE_TICK <= o < PRE_TICK_END for m, o in fs):
            return "other", None
        below = below_run_session(fs)
        if below is None:
            return "other", None
        if is_wait(below):
            return "pair-wait", None
        k = len(fs) - len(below)
        return "pair-run", frames(i)[k + 1] if len(below) > 1 else frames(i)[k]

    def ot_waiting(i):
        fs = [parse_frame(f) for f in frames(i)]
        below = below_run_session(fs)
        return below is not None and is_wait(below)

    gts = [g[0] for g in gt]
    cls = Counter()
    run_cmds = Counter()
    ot_incl, ot_leaf, ot_path = Counter(), Counter(), Counter()
    ot_state = Counter()
    paired = 0
    for t, s in ot:
        j = bisect.bisect_left(gts, t)
        best = None
        for k in (j - 1, j):
            if 0 <= k < len(gt) and abs(gts[k] - t) <= 0.6 and (best is None or abs(gts[k] - t) < abs(gts[best] - t)):
                best = k
        if best is None:
            continue
        paired += 1
        c, cmd = gt_class(gt[best][1])
        w = ot_waiting(s)
        cls[c] += 1
        ot_state[(c, "OT waits" if w else "OT works")] += 1
        if c == "pair-run" and cmd:
            run_cmds[label(cmd, names)] += 1
        if c == "pair-wait" and not w:
            fr = frames(s)
            seen = set()
            for f in fr:
                l = label(f, names)
                if l not in seen:
                    ot_incl[l] += 1
                    seen.add(l)
            exe_frames = [f for f in fr if f.startswith(EXE)]
            if exe_frames:
                ot_leaf[label(exe_frames[-1], names)] += 1
            ot_path[" > ".join(label(f, names).split(" ")[0] for f in exe_frames[-4:])] += 1

    n = paired
    print(f"# {os.path.basename(os.path.normpath(a.run))} / {scene}: {n} paired sweeps"
          + (f" over {(t1 - t0) / 1000:.1f} s" if t0 is not None else ""))
    print()
    print("| game thread | share | OT works | OT waits |")
    print("|---|---|---|---|")
    for c in ("pair-wait", "pair-run", "other"):
        print(f"| {c} | {100 * cls[c] / n:.1f} % | {100 * ot_state[(c, 'OT works')] / n:.1f} % | "
              f"{100 * ot_state[(c, 'OT waits')] / n:.1f} % |")
    if run_cmds:
        print("\n## pair-run: the commands the GT runs for the OT (frame below the step)\n")
        tot = sum(run_cmds.values())
        for k, v in run_cmds.most_common(15):
            print(f"- {100 * v / tot:5.1f} %  {k}")
    m = sum(ot_leaf.values())
    if m:
        print(f"\n## OT while the GT waits for it ({m} sweeps): inclusive\n")
        for k, v in ot_incl.most_common(a.top):
            print(f"- {100 * v / m:5.1f} %  {k}")
        print("\n## leaf (innermost exe function)\n")
        for k, v in ot_leaf.most_common(a.top):
            print(f"- {100 * v / m:5.1f} %  {k}")
        print("\n## innermost 4 exe frames (outer > inner)\n")
        for k, v in ot_path.most_common(25):
            print(f"- {100 * v / m:5.1f} %  {k}")


if __name__ == "__main__":
    main()
