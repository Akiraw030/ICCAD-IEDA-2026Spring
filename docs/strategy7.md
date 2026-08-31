# Strategy 7: Transactional Timing Engine Completion

> Strategy7 is now the completed timing-engine and pipeline history. The current
> post-FinalAlt perturb-and-recover work is Strategy8; see `docs/strategy8.md`
> and `docs/pipeline.md`.

## Goal

Strategy 7 keeps the Strategy 6 optimization policy and focuses on the
`FastTimingEngine` hot path. The main goals are:

1. make rejected transactions proportional to the affected path groups instead
   of all timing groups;
2. reuse immutable path topology across synchronization;
3. expose enough path-level information for Final Alternating Greedy repair to
   use the fast engine;
4. preserve periodic full synchronization and final full validation.

When this document differs from an older strategy document, the source of truth
is:

```text
source code > pipeline.md > strategy7.md > strategy6.md > older strategy documents
```

## Historical Strategy 7 status (2026-07-28)

Strategy 7 originally focused on completing the transactional timing engine.
The values in this section are the final Strategy7-era configuration and are
kept for experiment history; they are **not** the current production defaults.
Strategy8 subsequently changed the pipeline and acceptance policy.

The current default flow is documented in `docs/pipeline.md` and
`docs/strategy8.md`:

```text
Phase0
  -> Repair/Reclaim cycles
  -> Final Alternating Greedy
  -> Strategy8 Perturb-and-Recover
  -> full timing/area/legality validation
```

For reference, the following were the important Strategy7-era defaults:

```text
Global time                    = 570 seconds
alpha / beta / gamma           = 0.40 / 0.30 / 0.30
Phase0 score acceptance        = On
Repair/Reclaim score acceptance= Off (historical; current default is On)
FinalAlt score acceptance      = Off (optional)
Repair/Reclaim absolute end    = 360 seconds
Per-cycle reclaim budget       = 120 seconds
Repair split depth             = 2
Repair failure recovery        = On
FinalAlt cooldown / failures   = 3 / 3
FinalAlt repair/reclaim types  = 3 / 3
```

### Strategy7 Phase0 follow-up

The late-Strategy7 pipeline used individual FastTimingEngine resize trials by
default. Automatic batching remained available but disabled. Its settings were:

- unlimited candidate count (`phase0_max_trial_nodes=0`);
- 180-second safety budget;
- six types per node;
- five pressure-recompute passes;
- estimated-value node ordering;
- weighted-score move acceptance using `0.40 / 0.30 / 0.30`.

The earlier fixed 11,448-node budget described below is retained only as a
historical engine A/B setup; it is not the current default.

### Strategy7 Repair/Reclaim follow-up

Rejected Repair Pulse batches are recursively split to depth 2. If every split
branch fails, failure recovery lowers the requested-delay scale and temporarily
blacklists failed targets. In this historical TNS-only mode, Repair acceptance
used guarded OR:

- while both corners are open, either corner TNS may provide the improvement;
- after FF TNS reaches zero, FF must remain closed and SS TNS must strictly
  improve.

Reclaim retained the original area/pressure heuristic and timing-giveback
policy. Strategy8 later adopted score-based Repair/Reclaim acceptance while
keeping this heuristic ordering; see `docs/pipeline.md` for the current rule.

### FinalAlt search-space expansion

Three extensions are now part of the default FinalAlt pipeline:

1. a failed repair target is retried after a three-iteration cooldown;
2. a target becomes permanently blacklisted after three failed retries;
3. each repair target tries up to three buffer types, and each reclaim node
   exposes up to three smaller types.

Multi-type repair evaluates all candidates transactionally, rolls every trial
back, and then reruns and commits the best acceptable type. Heuristic mode
chooses the lowest timing cost; optional score mode chooses the highest weighted
score. Reclaim candidates are revalidated against the node's current type, so a
stale candidate produced before another accepted resize is safely rejected.

When an iteration inserts no repair buffer only because candidates are waiting
for cooldown expiry, it now skips the full reclaim scan and advances directly to
the next outer iteration. This removed 60--70 seconds of empty scanning per
large-case cooldown iteration.

### FinalAlt A/B results

All testcase4 variants below entered FinalAlt from the same state:

```text
SS TNS/WNS/NVP = -0.4814 / -0.0216 / 261
FF TNS/WNS/NVP = 0 / 0 / 0
Area           = 5674.076
```

| testcase4 final metric | Original heuristic | Expanded heuristic | Expanded + score objective |
|---|---:|---:|---:|
| SS TNS | -0.1314 | -0.0468 | -29.7840 |
| SS WNS | -0.0216 | -0.0027 | -0.0128 |
| SS NVP | 68 | 58 | 4,765 |
| Area | 5608.860 | 5608.400 | 4756.082 |
| Weighted score | 1.525774 | 1.538533 | 1.550768 |
| Total runtime | 181.5129 s | 289.5563 s | 185.3599 s |
| FinalAlt runtime | 43.391 s | 156.898 s | 55.251 s |
| Repair insertions | 235 | 261 | 7 |
| Accepted reclaim moves | 542 | 553 | 3,908 |

Compared with the original heuristic, the expanded heuristic improves weighted
score by 0.84%, reduces SS TNS magnitude by 64.38%, reduces WNS magnitude by
87.50%, and reduces NVP by 14.71%. Runtime increases by 59.52%. This tradeoff was
accepted, so the expanded search is now the Strategy 7 default.

On testcase0, the same expansion changes the final result from score `0.953516`,
SS TNS/WNS `-0.0109 / -0.0047`, 10 NVP, and area `125.741` to score `0.960800`,
SS TNS/WNS `-0.0062 / -0.0031`, 9 optimizer-reported NVP, and area `121.365`.

### Optional FinalAlt score objective

`enable_final_alt_score_based_acceptance` remains a separate option and is
disabled by default. With it enabled, repair requires a strict score increase
and reclaim requires a non-decreasing score.

The testcase4 score experiment reaches the highest weighted score and reduces
area by about 15.20% versus the expanded heuristic, but SS TNS magnitude grows
about 636 times and NVP grows about 82 times. The current `gamma=0.30` can reward
area reduction enough to accept many small timing violations. Pure score
acceptance is therefore not suitable as the default without an additional TNS
or violation-count guard.

### Documentation and residual-bug audit

The current `README.md`, `pipeline.md`, and `OptimizerConfig` were compared
parameter by parameter. Stale references to a 225-second Repair/Reclaim
boundary, split depth 3, and a 20-second FinalAlt local budget were corrected to
the current 360-second boundary, depth 2, and disabled local budget. Historical
A/B tables are now labeled as historical rather than current defaults.

The FinalAlt cooldown, multi-type selection, transaction rollback, stale reclaim
candidate handling, and time-limit paths were reviewed. No tree, timing-cache,
area, or legality synchronization bug was found. Unused legacy helpers and
variable-shadow compiler warnings were removed without changing optimizer
policy. The only remaining observed discrepancy is the near-zero NVP
classification described in the Verification section.

## Timing-engine coverage

The optimizer mutates the clock tree through three operation families:

| Mutation | Fast trial | Tree commit | Rollback |
|---|---:|---:|---:|
| buffer resize | yes | yes | yes |
| insert one buffer between a parent and child | yes | yes | yes |
| delete a non-original buffer and reconnect its children | yes | yes | yes |

These cover every mutation currently used in the default flow:

```text
Phase0 resize
Repair/Reclaim insert, resize, delete
FinalAlt repair insert
FinalAlt reclaim resize, delete
```

The engine's interval representation remains valid for these operations because
none of them changes the set or DFS order of FF sinks. Inserted buffers inherit
the child's sink interval; deleted-buffer intervals disappear; ancestor
intervals stay unchanged. `sync_from_tree()` still detects a changed FF list and
rebuilds the static path topology when needed.

Operations outside that contract, such as adding/removing FFs, arbitrary
reparenting, or changing timing-path input data, require a full sync/rebuild and
are not exposed as transactions.

## Changes from Strategy 6

### Local rollback

Strategy 6 already saved the affected arrivals and timing groups in each
transaction, but rollback restored those entries and then cleared and rebuilt
both global violation sets by scanning every timing group.

Strategy 7 removes that redundant global rebuild. The rollback now:

1. removes each affected group's trial contribution;
2. restores its old setup/hold slack;
3. restores the old contribution;
4. restores affected arrivals, aggregate timing, and area.

This changes rejected-trial rollback from `O(all path groups)` to
`O(affected path groups)`.

### Static path reuse

`sync_from_tree()` no longer rebuilds the launch/capture path-group topology when
the ordered FF list is unchanged. It still rebuilds node ranges, arrivals, area,
and all group slacks, so each optimization cycle starts from a canonical full
engine state.

Full per-cycle synchronization was deliberately retained. An experiment that
skipped it was faster, but tiny floating-point drift changed later candidate
ordering and made large-case quality less stable.

### FinalAlt repair on FastTimingEngine

The engine now provides `worst_violation(blacklist)`, returning:

- setup or hold;
- launch FF and capture FF;
- mutation target;
- violation slack.

FinalAlt repair uses this query and `trial_insert_between()`. After an accepted
repair batch, the path-detail cache is rebuilt once for candidate ranking,
instead of after every accepted insertion.

The path-detail cache is still retained because pressure/ranking code needs more
information than aggregate TNS/WNS/NVP.

### Transaction hot-path refinement

The follow-up refinement remains part of Strategy 7.

- SS and FF arrival rollback entries are combined into one snapshot record.
- Hot optimizer loops reuse `Transaction` vector capacity.
- A generation/epoch array replaces the per-trial `unordered_set` used to
  deduplicate affected path groups.
- Reports now include affected-arrival/group counts and collection/update time.

On testcase4, 290,027 fast trials touch about 13.0 million arrival snapshots and
73.3 million affected-group entries. Avoiding repeated hashing and allocation
therefore has a measurable effect.

### Historical deterministic Phase0 work budget

Phase0 previously stopped only at a 90-second boundary. A faster engine or a
different build mode changed how many candidates were processed and could send
Repair/Reclaim down a very different trajectory.

The original Strategy 7 engine A/B used a total Phase0 budget of 11,448
candidate nodes and a 120-second safety limit. The candidate budget was shared
across all passes rather than being reset per pass.

This value matches the work completed by the previous testcase4 Strategy7
baseline. Fixed-work A/B runs therefore produce identical Phase0, Repair/Reclaim,
FinalAlt, and final output structures.

The current pipeline no longer uses this fixed-work setting. Its default is an
unlimited count, a 180-second safety budget, six types per node, and five passes.

## Verification

The original engine work and the current pipeline were checked at several
levels:

1. `enable_fast_timing_verify=true` with interval `1` on testcase0. Every
   committed fast transaction matched full `DelayModel` TNS, WNS, and area.
2. Incremental area verification and per-FinalAlt-commit full legality
   verification passed under the current cooldown/multi-type defaults.
3. A warning-enabled build and ASan/UBSan testcase0 run passed. LeakSanitizer
   could not run in the current ptrace-restricted environment.
4. Final internal full timing and legality validation reported `legality=OK`.
5. The official checker reported zero floating nodes, multi-drive nodes, illegal
   buffers/FFs/fanouts, mismatched nodes, and illegal timing paths for the
   current testcase0 output.

Near zero, the engine, final report, and official checker can disagree on NVP by
a few paths due to floating-point sign classification. In the current testcase0
validation, the optimizer reports 9 SS violations and the official checker
reports 6, while both report SS TNS/WNS `-0.0062 / -0.0031`. This does not affect
the current score, area, legality, TNS, or WNS. A common zero-slack epsilon should
be introduced before violation count is used in future acceptance decisions.

## Transaction-refinement benchmark

The fair A/B build uses `ENABLE_OUTPUT=ON` without specifying
`CMAKE_BUILD_TYPE`, matching the previous Strategy7 build. Runs were sequential.
The internal equal-weight score is omitted because the official coefficients
are unknown.

| Case | Version | Runtime (s) | SS TNS | SS WNS | SS NVP | FF TNS | Area |
|---|---|---:|---:|---:|---:|---:|---:|
| testcase0 | before refinement | 0.5611 | -0.0161 | -0.0068 | 8 | 0 | 141.796 |
| testcase0 | current Strategy7 | 0.4112 | -0.0161 | -0.0068 | 8 | 0 | 141.796 |
| testcase4 | fixed-work baseline | 175.4031 | -0.4711 | -0.0336 | 166 | 0 | 5329.233 |
| testcase4 | current Strategy7 | 158.4825 | -0.4711 | -0.0336 | 166 | 0 | 5329.233 |

Measured improvements:

- testcase0 total runtime: `-26.7%` (`1.36x` faster).
- testcase0 fast-trial time: `0.2934 -> 0.1433 s` (`-51.2%`).
- testcase4 total runtime: `-9.65%` (`1.11x` faster).
- testcase4 fast-trial time: `35.2593 -> 17.5194 s` (`-50.3%`).
- testcase4 reclaim trial loop: `40.5367 -> 22.8467 s` (`-43.6%`).
- testcase4 Repair/Reclaim stage: `62.5053 -> 44.5927 s` (`-28.7%`).
- Final QoR and emitted structures are identical in the fixed-work A/B.

With a Release (`-O3`) build, the same current Strategy7 structures are produced
in `0.1084 s` for testcase0 and `94.3065 s` for testcase4. These numbers are
reported separately because compiler optimization is not part of the engine-only
A/B.

The raw reports for this historical engine-only table are intentionally omitted
from the public repository. The aggregate final benchmark is retained in
[`benchmark_results.md`](benchmark_results.md).

## Refactor decision

`optimizer.cpp` is now roughly 8,200 lines, so further development would benefit
from a targeted refactor. A broad rewrite should not precede the next QoR
experiment because it would make timing regressions harder to attribute.

The recommended next refactor is mechanical:

1. move Phase0, Repair/Reclaim, and FinalAlt into separate implementation units;
2. extract shared trial acceptance and verification helpers;
3. add direct transaction tests for resize/insert/delete commit and rollback;
4. keep optimizer policy and default parameters unchanged during that work.

The Strategy 7 changes themselves are localized and do not require deleting
historical fallback paths yet. Those paths remain useful as correctness oracles
while the timing engine is being extended.
