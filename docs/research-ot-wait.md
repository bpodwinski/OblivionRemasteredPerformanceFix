# Old-engine wait investigation

Goal: identify work on OblivionThread that delays its pairing with Unreal's GameThread. This is a measurement-first follow-up to the perception optimization (upstream PR #55) and the horse-corral challenge (upstream issue #32). No executable patch, DLL change or performance claim is made by this diagnostic.

## Baseline

Start from the 1.6.0-beta-derived fork. Existing OtFrameTrim and LooseMissCache already target specific old-engine costs; do not duplicate them. First reproduce a long pairing wait and locate its dominant stacks. A stack sampled during a high-wait frame is a lead, not necessarily the lock owner or the cause.

## Capture (Windows, installed Steam game and benchmark prerequisites)

From the repository root, close any existing benchmark session normally. Install the matching fix DLL/INI and set up the existing benchmark tools as described in AGENTS.md and CONTRIBUTING.md. Keep the default fixes enabled and use a fresh benchmark process. Do not replace an existing personal capture: archive any existing ledger before starting.

```powershell
. .\tools\GamePaths.ps1
$game = Get-OblivionGame
.\tools\Run-Benchmark.ps1 -Label ot-corral -Scenario challenges -Scenes ic-waterfront-pass,brindle-corral-walk -Profile small -Pass perf -Session fresh -After close -FixIni @{ HitchLedger = 1; HitchLedgerMs = 5 } -Stacks GameThread,OblivionThread -StackSampler fast -StackIntervalMs 2
```

The selected walk is a supported stack-sampling scene. Static holds are not sampled by this runner. The two selected scenes already occur in this order in challenges.json, so this command does not depend on the scene-order patch proposed in upstream PR #60.

Immediately archive the ledger and game log, and retain the matching result.json and stacks folder. Use the actual run directory printed by the runner:

```powershell
$run = Get-ChildItem .\captures\bench -Directory | Where-Object { $_.Name -like '*-ot-corral' } | Sort-Object Name -Descending | Select-Object -First 1
Copy-Item (Join-Path $game.Win64 'OblivionPerfFix-ledger.csv') (Join-Path $run.FullName 'ledger.csv')
Copy-Item (Join-Path $game.Win64 'OblivionPerfFix.log') (Join-Path $run.FullName 'fix.log')
python .\tools\ot_wait_profile.py --run $run.FullName --scene brindle-corral-walk --ledger (Join-Path $run.FullName 'ledger.csv') --out (Join-Path $run.FullName 'ot-wait.md')
```

Use only a capture with no game restart during the selected scene. If the walk never enters the expensive state, repeat fresh runs or investigate another reproducible route; report failure to reproduce rather than inventing a cost. For frame-time comparisons, repeat the same route without stack sampling (suspending threads perturbs timing).

## Reading the report

- Frame cohorts use pair_ms from the existing HitchLedger: low <= 0.5 ms, high >= 2 ms, middle otherwise. Thresholds can be changed with --low / --high. Only recorded play frames fully within the result.json scene interval are considered, and the reported game time excludes the cap-sleep interval.
- The QPC stack samples are assigned to each frame's half-open work interval. Compare per-thread stack-sample shares and the signed difference in percentage points. The script reports coverage and frame-number gaps; no samples or omitted frames are imputed. HitchLedgerMs=5 still excludes sub-5 ms frames; keep the threshold unchanged throughout the capture.
- The analysis is over the whole work window, not the exact pairing-wait interval. A frequent waiting stack on GameThread is a symptom. Look at OblivionThread and the worker stacks (optional additional sampling) to find plausible owners or expensive work. Sample share is not CPU time, and the samples are not independent.
- A single-state capture is not a failed test: it establishes whether the other state occurred and lists observed stacks, but does not support an internal low/high comparison. Do not treat a missing cohort as zero.

## Next research gate

1. Reproduce the expensive state with the current upstream baseline; obtain repeated, unsampled timing captures and sampled stack reports.
2. Identify recurring old-engine RVAs, resolve their functions against the matching executable in Ghidra, and trace any shared data and callers. Confirm the candidate using targeted measurements; do not assume a stack appearing more often caused the delay.
3. Investigate pure computation, redundant updates, and avoidable synchronization before attempting concurrency. Do not run legacy gameplay mutations on arbitrary helper threads.
4. Only then implement a fail-closed, default-off fix with an equivalence argument, byte-pattern validation, self-tests and a verification mode when feasible. Compare A-B-B-A and B-A-A-B without the sampler; check game state, combat, transitions, and stability.

## Offline validation

```powershell
python -m unittest discover -s tools -p test_ot_wait_profile.py -v
```

The synthetic tests cover correct frame and stack alignment (including cap sleep and boundary timestamps), cohort classification, missing-cohort reporting, malformed stack indices, biased ledger thresholds and mixed sessions. They do not demonstrate any in-game speedup or verify the Windows binary.
