# Strategy 6: Fast Timing Integration and Runtime Cleanup

## Purpose

This document describes the **current implementation of `Optimizer::optimize` in `optimizer.cpp`**.

It is intended to replace / supersede `strategy5.md` as the working strategy document for the current runtime-optimized implementation.

Strategy 6 keeps the Strategy 5 architecture, but adds the FastTimingEngine integration and the second-round runtime cleanup work. The optimizer still uses default non-reset Phase 0, the Repair/Reclaim cycle stage after Phase0, and Final Alternating Greedy as the default final cleanup.

When this document differs from earlier strategy documents, the source of truth is:

```text
optimizer.cpp > strategy6.md > strategy5.md > strategy4.md > strategy3.md > strategy2.md
```

## High-Level Summary

The optimizer is a useful-skew clock-tree optimizer.

It modifies the given clock tree using three legal operation families:

1. **Insert new buffers** to increase clock delay on selected branches.
2. **Remove or merge non-original buffers** and reconnect their children.
3. **Change buffer cell type** to adjust delay / area.

The implementation is organized into Phase0, an optional repeated Repair/Reclaim cycle stage, and a configurable final cleanup stage:

```text
Phase 0 : Default non-reset buffer resizing with fast resize trials
Cycle   : Phase1A-pulse + pressure-guided reclaim with fast trials
Final   : Final Alternating Greedy by default, with fast reclaim trials
Legacy  : Phase1B + Phase2 when final_alt replacement is disabled in OptimizerConfig
```

The high-level intent is:

```text
First condition existing buffers with Phase0.
Then alternate coarse timing repair and area reclaim until the 8-minute mark.
Then use Final Alternating Greedy as the final repair/reclaim cleanup.
```

The current implementation is still partly oracle-driven, but the hot trial loops avoid full `analyze_timing(...)` where possible:

- Phase 0 uses FastTimingEngine resize trials by default.
- Phase 0 still uses full `DelayModel::analyze_timing(...)` at pressure-ranking boundaries when path-level timing details are required.
- Phase 0 does not rebuild the old incremental timing cache after every accepted fast resize.
- Repair/Reclaim can use FastTimingEngine for pulse insertions and reclaim resize/delete trials.
- Phase 1A uses full timing analysis for each batch.
- Final Alternating Greedy repair insertions use the path-preserving incremental timing path, because worst-path selection requires `path_results`.
- Final Alternating Greedy reclaim resize/delete trials use FastTimingEngine when enabled.
- Final Alternating Greedy keeps path-level timing for ranked reclaim candidate generation instead of replacing it with aggregate fast timing.
- Legacy Phase1B and Phase2 also use incremental clock-arrival / path-group updates when enabled.
- Finalization always recomputes full timing, area, legality, and score.

## Strategy 6 Speedup Notes

FastTimingEngine is an aggregate timing backend for local tree edits. It supports resize, insert, and delete transactions with commit/rollback, and reports SS/FF TNS/WNS/violation counts plus tracked area.

Important limitation:

```text
FastTimingEngine timing summaries do not carry per-path path_results.
```

Therefore, stages that need path-level information must keep or rebuild a path-detail timing result:

- Phase0 pressure ranking uses full `DelayModel::analyze_timing(...)` if the current fast timing summary has no `path_results`.
- Repair/Reclaim pressure-guided reclaim uses full `DelayModel::analyze_timing(...)` for pressure construction if a fast pulse insertion produced an aggregate-only timing result.
- FinalAlt repair insertion stays on the existing incremental path cache so `choose_worst_target(...)` can keep seeing individual path slacks.
- FinalAlt ranked reclaim candidate generation uses the path-detail cache for pressure ranking, then uses FastTimingEngine for the actual resize/delete trial acceptance.

Avoided runtime traps:

- Accepted Phase0 fast resizes no longer rebuild the old Phase0 incremental timing cache after every commit.
- FinalAlt repair fast insertions are disabled for now, because they forced a full path-cache rebuild after every accepted insertion.
- FastTimingEngine verification is configurable from `OptimizerConfig` and is off by default for normal runs.

## Inputs

`Optimizer::optimize` receives:

```cpp
OptimizationSummary Optimizer::optimize(
    ClockTree& tree,
    const std::vector<BufSpec>& libs,
    const std::vector<PathInfo>& ss_paths,
    const std::vector<PathInfo>& ff_paths,
    double clock_period,
    bool reset_phase0 = false,
    const std::string& phase0_branch_name = "normal"
)
```

### `tree`

The mutable clock tree. The optimizer modifies this object directly.

The tree contains:

- root clock source
- buffer nodes
- FF sink nodes
- parent / child relationships
- an index from node name to node pointer
- `original` flag for nodes parsed from the input tree

### `libs`

The buffer library.

Each `BufSpec` contains:

```cpp
struct BufSpec {
    std::string name;
    double width;
    double height;
    std::vector<double> ss_delay;
    std::vector<double> ff_delay;
};
```

Area is estimated as:

```cpp
area = width * height
```

Buffer delay depends on:

```text
cell type + fanout + corner
```

### `ss_paths`

Timing paths used for SS setup analysis.

SS corner is used to evaluate setup slack.

### `ff_paths`

Timing paths used for FF hold analysis.

FF corner is used to evaluate hold slack.

### `clock_period`

Clock period used to compute setup / hold slack.

The internal timing model uses:

```cpp
t_setup = 0.08 * clock_period;
t_hold  = 0.05 * clock_period;
```

## Outputs

The optimizer returns `OptimizationSummary`.

Important fields:

```cpp
bool success;
bool early_stopped;
bool phase0_enabled;
bool phase0_reset_experiment_enabled;
std::string message;
std::string phase1a_stop_reason;
TimingAnalysisResult final_timing;
ScoreMetrics final_score;
LegalityReport final_legality;
double final_area;
int iterations;
int phase1a_insertions;
int phase1b_insertions;
int phase2_removals;
int phase2_downsizes;
std::vector<StageSnapshot> phase1a_iteration_snapshots;
std::vector<StageSnapshot> phase2_iteration_snapshots;
StageSnapshot after_phase1a;
StageSnapshot after_phase1b;
StageSnapshot after_phase2;
Phase0Summary phase0;
std::vector<std::string> applied_moves;
```

### Note on counters

The current counters are useful for debugging, but some counters are not perfectly semantic:

- `phase2_downsizes` may also count `SizeSwap` and `Rebalance` moves, even though those are not always true area-reducing downsizes.
- Phase 1A accounting is transactional: trial insertions and move strings are stored locally and committed only after the batch passes its timing acceptance check.

The Phase 2 counter semantics remain a reporting limitation, not a tree-correctness issue.

---

# 1. Timing Model

## 1.1 Clock arrival computation

The optimizer uses `DelayModel` to compute FF clock arrival times.

For each corner:

```cpp
std::unordered_map<std::string, double> compute_clock_arrivals(
    const ClockTree& tree,
    bool ss_corner
)
```

The traversal starts from the root with arrival `0.0`.

For each buffer node:

```text
node_delay = buffer_delay(cell_type, fanout, corner)
next_arrival = arrival + node_delay
```

For each sink FF child:

```text
arrival_by_ff[ff_name] = accumulated arrival before the FF sink
```

Root delay is included only if root is represented as a non-sink node with a non-empty type. In the usual input, root is a clock source with no buffer type, so it contributes no delay.

## 1.2 Buffer delay lookup

For SS:

```cpp
buffer_delay_ss(cell_type, fanout)
```

For FF:

```cpp
buffer_delay_ff(cell_type, fanout)
```

If fanout is greater than the delay table length, the implementation clamps to the last entry:

```cpp
idx = min(fanout - 1, delays.size() - 1)
```

This means delay lookup itself does not reject oversized fanout. Legality checking is responsible for enforcing fanout limits.

## 1.3 Slack equations

For each timing path:

```text
skew = capture_clk_delay - launch_clk_delay
```

Setup slack:

```text
setup_slack = clock_period - t_setup - data_delay + skew
```

Hold slack:

```text
hold_slack = data_delay - t_hold - skew
```

Interpretation:

- Increasing capture clock delay improves setup slack.
- Increasing launch clock delay improves hold slack.
- Increasing capture clock delay hurts hold slack.
- Increasing launch clock delay hurts setup slack.

## 1.4 Path grouping by FF pair

Timing paths are grouped by:

```text
launch_ff -> capture_ff
```

The key is:

```cpp
launch_ff + "->" + capture_ff
```

For all SS paths with the same FF pair:

```text
setup_slack = minimum setup slack among those paths
```

For all FF paths with the same FF pair:

```text
hold_slack = minimum hold slack among those paths
```

Therefore, the optimizer generally works on merged FF-pair timing results rather than every raw path line independently.

## 1.5 TNS / WNS convention

Negative slack is bad.

For setup:

```text
SS TNS = sum of negative setup slacks
SS WNS = most negative setup slack
```

For hold:

```text
FF TNS = sum of negative hold slacks
FF WNS = most negative hold slack
```

If there is no violation in a corner:

```text
TNS = 0
WNS = 0
```

Since TNS and WNS are negative-or-zero values, larger is better:

```text
-0.01 is better than -0.10
0.00 is best
```

---

# 2. Common Helper Rules

## 2.1 `is_buffer_node`

A node is considered a buffer node if:

```cpp
node != nullptr
&& !node->is_sink
&& !node->type.empty()
```

This excludes FF sinks and empty-type nodes.

## 2.2 `timing_not_worse`

This helper checks whether a trial timing result is no worse than the current one.

The rule is:

```cpp
trial.ss.tns >= current.ss.tns - eps
trial.ff.tns >= current.ff.tns - eps
trial.ss.wns >= current.ss.wns - eps
trial.ff.wns >= current.ff.wns - eps
```

Default epsilon:

```cpp
eps = 1e-9
```

Meaning:

```text
SS TNS cannot degrade.
FF TNS cannot degrade.
SS WNS cannot degrade.
FF WNS cannot degrade.
```

This helper is mainly used in Phase 2 and related trial helpers.

## 2.3 `timing_strictly_better`

A trial is strictly better if any one of the four timing metrics improves:

```cpp
trial.ss.tns > current.ss.tns + eps
|| trial.ff.tns > current.ff.tns + eps
|| trial.ss.wns > current.ss.wns + eps
|| trial.ff.wns > current.ff.wns + eps
```

This function exists as a helper. Phase 1B implements a similar rule directly.

## 2.4 `buffer_nominal_delay`

Nominal delay is computed as the average of SS and FF delay at a given fanout:

```cpp
0.5 * (ss_delay + ff_delay)
```

It is used for buffer selection tie-breaking.

## 2.5 `choose_smallest_buffer`

This is used to choose the fallback insertion buffer.

Selection rule:

1. Choose the smallest area buffer.
2. If area ties, choose the smaller nominal delay at fanout 1.

This fallback buffer is used when adaptive buffer selection cannot find a buffer whose delay fits the required target delay.

## 2.6 `choose_batch_buffer`

This helper is defined inside `optimize`.

Input:

```cpp
double target_delay
```

Selection rule:

1. For each buffer type, compute nominal delay at fanout 1.
2. Skip the buffer if nominal delay is greater than `target_delay`.
3. Among candidates with delay <= target delay, choose the largest delay.
4. If delay ties, choose smaller area.

In short:

```text
choose the strongest fitting delay that does not overshoot required delay
```

If no buffer delay is <= target delay, it returns `nullptr`.

Phase 1B then falls back to `smallest_buffer` if this happens.

## 2.7 `choose_next_smaller_buffer`

This helper is used for downsizing in Phase 2.

Input:

```cpp
current_type
fanout
```

Selection rule:

1. Find current buffer library entry.
2. Compute current area.
3. Consider only candidates with smaller area than current.
4. Candidate must support the fanout according to its delay table check.
5. Choose the candidate with the largest area below current area.
6. If area ties, choose smaller nominal delay.

Interpretation:

```text
Pick the next conservative downsize step, not necessarily the smallest possible cell.
```

Current caveat:

```text
The implementation checks SS delay table length. It should ideally also check FF table length.
```

## 2.8 `choose_medium_buffer`

This helper is used by the Phase 2 `Rebalance` pattern.

Inputs:

```cpp
fanout_a
fanout_b
```

Selection rule:

1. Collect candidate buffers that support both fanouts.
2. Compute all candidate areas.
3. Compute median area.
4. Choose the candidate whose area is closest to the median.
5. If distance ties, choose smaller nominal delay at max fanout.

Interpretation:

```text
Pick a medium-sized buffer for replacing an imbalanced pair.
```

## 2.9 `is_physically_smaller`

This helper compares two buffer library cells.

Primary rule:

```text
smaller area => physically smaller
```

Tie-break:

```text
if area equal, larger delay is considered physically smaller
```

This is used by `SizeSwap` to determine whether the upper buffer is smaller than the lower buffer.

## 2.10 `choose_worst_target`

This helper selects the next violating target for Phase 1B.

For every merged timing result:

```cpp
setup_bad = setup_slack < 0.0
hold_bad  = hold_slack < 0.0
```

If neither is bad, skip.

If setup is worse or the only violation:

```text
target = capture_ff
reason = setup
violation_slack = setup_slack
```

If hold is worse or the only violation:

```text
target = launch_ff
reason = hold
violation_slack = hold_slack
```

If both setup and hold are bad, it chooses the more negative slack.

Targets already in the blacklist are skipped.

The selected target is the remaining path endpoint with the most negative violating slack.

---

# 3. Structural Modification Helpers

## 3.1 `insert_buffer_between`

This tree helper inserts a new buffer between an existing parent and child.

Before:

```text
parent -> child
```

After:

```text
parent -> NEW_BUF_X -> child
```

The new buffer:

```text
is_sink = false
original = false
type = selected buffer type
name = generated unique name
```

The original child keeps its name and subtree.

## 3.2 `remove_buffer_node`

This helper removes a non-original buffer node and reconnects its children to its parent.

Before:

```text
parent -> buffer -> child_0, child_1, ...
```

After:

```text
parent -> child_0, child_1, ...
```

The removed buffer state is stored in `RemovedBufferState`, including:

```cpp
name
type
parent_name
child_names
parent_index
original
```

Important rule:

```text
Original buffers cannot be removed.
```

If the target node is original, removal fails.

## 3.3 `restore_buffer_node`

This helper restores a previously removed buffer using `RemovedBufferState`.

It:

1. Finds the saved parent.
2. Finds the children that were moved to the parent.
3. Creates the removed buffer again.
4. Moves those children back under the restored buffer.
5. Inserts the restored buffer back near its original parent index.
6. Re-adds it to the tree index.

This is used for rollback after a rejected removal trial.

## 3.4 `set_buffer_type`

This changes a buffer node's cell type.

Current implementation allows both:

```text
original buffers
newly inserted buffers
```

to change type.

It does not rename or delete the node.

This is important because changing the type of an original buffer is legal, while deleting or renaming the original buffer is not.

---

# 4. Incremental Timing Cache

Phase 1B and Phase 2 use an incremental cache to avoid full timing analysis for every trial.

## 4.1 Cache structure

```cpp
struct Phase1bTimingCache {
    TimingAnalysisResult timing;
    std::vector<Phase1bPathGroup> groups;
    std::unordered_map<std::string, std::vector<size_t>> groups_by_ff;
    std::multiset<double> ss_violations;
    std::multiset<double> ff_violations;
};
```

Despite the name `Phase1bTimingCache`, this cache is also reused in Phase 2.

## 4.2 Path groups

Each `Phase1bPathGroup` contains:

```cpp
std::vector<const PathInfo*> ss_paths;
std::vector<const PathInfo*> ff_paths;
```

Groups are keyed by:

```text
launch_ff -> capture_ff
```

Each group recomputes one merged `TimingPathResult`.

## 4.3 FF-to-group index

The cache builds:

```cpp
groups_by_ff[ff_name] -> list of group indices affected by this FF
```

This allows trial updates to recompute only timing path groups connected to changed FF arrivals.

## 4.4 Violation multisets

The cache maintains:

```cpp
ss_violations
ff_violations
```

These store negative slacks.

The WNS can be refreshed by taking the smallest value:

```cpp
wns = violations.empty() ? 0.0 : *violations.begin()
```

## 4.5 Incremental arrival update

When a trial changes a subtree, the optimizer calls:

```cpp
update_subtree_arrivals(model, node, corner, arrival_map, rollback)
```

The helper:

1. Computes the input arrival at the changed node.
2. DFS traverses the subtree.
3. Updates arrival values for FF sinks under that subtree.
4. Stores old values in rollback state.

Rollback is possible through:

```cpp
rollback_arrivals(arrival_map, rollback)
```

## 4.6 Incremental path update

After arrival updates:

1. Find affected path groups from changed FF names.
2. Save old `TimingPathResult` values.
3. Remove old path contribution from TNS/WNS summary.
4. Recompute affected groups.
5. Add new contribution.
6. Refresh WNS.

Rollback is possible through:

```cpp
rollback_path_groups(cache, path_rollback)
```

---

# 5. Initialization

At the beginning of `optimize`:

## 5.1 Empty tree guard

If `tree.root` does not exist:

```text
Optimization skipped: tree has no root.
```

The summary is returned immediately.

## 5.2 Build delay model

```cpp
DelayModel model(libs);
```

## 5.3 Start timer

The optimizer uses the configured conservative wall-clock limit:

```cpp
OptimizerConfig::time_limit_seconds = 570;
```

The helper:

```cpp
is_time_up()
```

returns true if elapsed time exceeds 570 seconds. The timer starts before Phase 0, so Phase0, Repair/Reclaim cycles, Final Alternating Greedy, and the legacy Phase1A/Phase1B/Phase2 path consume one shared budget.

## 5.4 Run Phase0 when enabled

When `cfg.enable_phase0` is true, the optimizer first runs non-reset Phase0 on the output-producing tree:

```cpp
summary.phase0 = run_phase0_timing_conditioning(
    tree, model, ss_paths, ff_paths, clock_period,
    baseline, summary, start_time, reset_phase0, phase0_branch_name);
```

Phase0 captures the original baseline snapshot and sets `baseline.valid = true`. It uses the same `start_time` and global wall-clock budget as the Repair/Reclaim cycles, Final Alternating Greedy, and the legacy Phase1A/Phase1B/Phase2 path.

## 5.5 Choose smallest fallback buffer

```cpp
const BufSpec* smallest_buffer = choose_smallest_buffer(libs, model);
```

If no buffer exists:

```text
Optimization skipped: no buffers available in library.
```

## 5.6 Initialize baseline when Phase0 did not already do it

If `baseline.valid == false`:

```cpp
baseline.timing = model.analyze_timing(tree, ss_paths, ff_paths, clock_period);
baseline.area = model.compute_tree_area(tree);
baseline.valid = true;
```

The baseline is used for:

- improvement threshold in Phase 1A
- final score computation
- report comparison

With the current default configuration, this fallback is usually skipped because Phase0 initializes the baseline before the Repair/Reclaim stage.

## 5.7 Compute Phase 1A plateau threshold

```cpp
baseline_total_tns_abs = abs(baseline.ss.tns) + abs(baseline.ff.tns)
improvement_threshold = baseline_total_tns_abs * 0.001
```

Interpretation:

```text
0.1% of original total absolute TNS
```

If a Phase 1A batch improves TNS by less than this positive threshold, Phase 1A stops and hands off to Phase 1B.

## 5.8 Create blacklist

```cpp
std::set<std::string> blacklist;
```

The blacklist is used only in Phase 1B to avoid retrying targets that previously failed trial insertion.

---

# 6. Stage Snapshot System

The optimizer records snapshots for debugging and reporting.

When `cfg.enable_timing_cache_verify` is true, snapshots also run a full timing-cache consistency check. Cached and full SS/FF TNS, WNS, and violation counts are compared using epsilon `1e-6`; a mismatch prints the snapshot context and aborts.

## 6.1 `capture_stage`

Captures:

```text
stage name
added buffer count
removed buffer count
downsized buffer count
runtime seconds
full timing analysis
area
legality report
```

It uses full `model.analyze_timing(...)`, not incremental cache.

## 6.2 `capture_phase1a_iteration`

For each Phase 1A iteration, it records:

```text
Phase 1A iter N
accepted Phase 1A insertion count accumulated before that iteration
full timing
area
legality
```

These snapshots are stored in:

```cpp
summary.phase1a_iteration_snapshots
```

## 6.3 `capture_phase2_iteration`

For each Phase 2 pattern pass, it records:

```text
Phase 2 iter N [PatternName]
removed count for the pattern
downsized count for the pattern
cached incremental timing
area
legality
```

These snapshots are stored in:

```cpp
summary.phase2_iteration_snapshots
```

With `cfg.enable_timing_cache_verify` true, the cached snapshot timing is immediately verified against a full `analyze_timing(...)` result.

---

# 7. Phase 1A — Coarse Batched Timing Fixing

## 7.1 Goal

Phase 1A tries to quickly reduce large timing violations by inserting buffers on a batch of structurally independent FF endpoints.

It is coarse because:

- it handles many targets per iteration,
- it verifies only after the whole batch,
- it accepts based on TNS non-degradation,
- it intentionally leaves finer cleanup to Phase 1B.

## 7.2 Loop limit

```cpp
const int max_iterations_phase1a = 100;
```

## 7.3 Per-iteration time check

At the start of each iteration:

```cpp
if (is_time_up()) {
    summary.early_stopped = true;
    summary.message = "Optimization stopped early due to time limit in Phase 1A.";
    break;
}
```

## 7.4 Full timing analysis

Each Phase 1A iteration computes:

```cpp
current_timing = model.analyze_timing(tree, ss_paths, ff_paths, clock_period);
summary.iterations += 1;
```

It then captures an iteration snapshot.

## 7.5 Stop if timing is clean

If both SS and FF TNS are zero:

```cpp
if (current_timing.ss.tns == 0.0 && current_timing.ff.tns == 0.0) break;
```

This means no negative setup slack in SS and no negative hold slack in FF.

## 7.6 Rank paths by worst slack

Phase 1A builds a list of all merged timing path results and sorts them by:

```text
min(setup_slack, hold_slack)
```

Most negative path comes first.

Tie-break:

```text
path_name lexicographic order
```

## 7.7 Build independent target batch

For each sorted path:

```cpp
setup_bad = path.setup_slack < 0.0;
hold_bad  = path.hold_slack < 0.0;
```

If neither is bad, skip.

If setup is chosen:

```text
target_node = capture_ff
required_delay = -setup_slack
```

If hold is chosen:

```text
target_node = launch_ff
required_delay = -hold_slack
```

The path chooses setup if:

```cpp
setup_bad && (!hold_bad || setup_slack <= hold_slack)
```

Meaning:

```text
choose setup if it is the only violation or it is at least as bad as hold
```

### Independence rule

A target is only added if both endpoints are unused in the current batch:

```text
launch_ff not in used_ffs
capture_ff not in used_ffs
```

After adding a path to the batch:

```text
used_ffs.insert(launch_ff)
used_ffs.insert(capture_ff)
```

This avoids applying multiple batch changes that affect the same FF pair neighborhood in the same Phase 1A iteration.

### Required delay per target

For each target node, Phase 1A records the maximum required delay seen:

```cpp
target_delays[target_node] = max(existing, required_delay)
```

## 7.8 Empty batch handling

If no target can be selected:

```text
Phase 1A stops and hands off to Phase 1B.
```

The variable `phase1a_broke_due_to_entanglement` may be set, but the current implementation does not use it afterward.

## 7.9 Insert buffers for the batch

For each target in the batch:

1. Check time limit.
2. Find the target node.
3. Find its parent.
4. Choose buffer type using `choose_batch_buffer(required_delay)`.
5. If no buffer fits, skip this target.
6. Generate a `NEW_BUF` name.
7. Insert buffer between parent and target.
8. Record inserted buffer name.
9. Store the insertion count and move description locally for this trial batch.

Current move description form:

```text
Phase1A insert NEW_BUF_X above TARGET
```

## 7.10 If no buffer was inserted

If the entire batch inserted zero buffers:

```text
Phase 1A stops and hands off to Phase 1B.
```

## 7.11 Verify batch by full timing analysis

After inserting all buffers in the batch:

```cpp
batch_timing = model.analyze_timing(tree, ss_paths, ff_paths, clock_period);
```

## 7.12 Phase 1A acceptance rule

Current Phase 1A accepts the batch if both corner TNS do not degrade:

```cpp
batch_timing.ss.tns >= current_timing.ss.tns
batch_timing.ff.tns >= current_timing.ff.tns
```

Important:

```text
Current Phase 1A does not check WNS in the batch acceptance rule.
```

Therefore, it is possible for Phase 1A to accept a batch that preserves or improves TNS while worsening WNS.

## 7.13 Batch rollback

If either SS TNS or FF TNS degrades:

1. For each inserted buffer, call `remove_buffer_node(...)`.
2. Stop Phase 1A.
3. Hand off to Phase 1B.

Message:

```text
Phase 1A batch worsened a corner TNS; reverting and handing off to Phase 1B.
```

```text
The tree is rolled back and local trial accounting is discarded.
No rejected insertion remains in phase1a_insertions or applied_moves.
```

## 7.14 Plateau detection

After an accepted batch:

```text
summary.phase1a_insertions += local accepted insertion count
append local batch move strings to summary.applied_moves
```

```cpp
tns_improvement =
    (batch_timing.ss.tns - current_timing.ss.tns)
  + (batch_timing.ff.tns - current_timing.ff.tns);
```

If:

```cpp
tns_improvement > 0.0
&& tns_improvement < improvement_threshold
```

Phase 1A stops and hands off to Phase 1B.

Message:

```text
Phase 1A plateaued (TNS improvement < 0.1% of baseline); handing off to Phase 1B.
```

Important caveat:

```text
If improvement is exactly 0, this condition does not stop Phase 1A.
```

## 7.15 End of Phase 1A

After Phase 1A stops, the optimizer captures:

```cpp
summary.after_phase1a
```

with full timing, area, legality, and runtime.

---

# 8. Phase 1B — Fine Greedy Cleanup

## 8.1 Goal

Phase 1B performs one-buffer-at-a-time greedy timing fixing.

It is finer than Phase 1A because:

- it selects one worst target at a time,
- it verifies each insertion immediately,
- it requires TNS and WNS to not degrade,
- it requires at least one timing metric to improve,
- it uses a blacklist to avoid repeating failed targets.

## 8.2 Build incremental cache

Before the loop:

```cpp
phase1b_ss_arrival = model.compute_clock_arrivals(tree, true);
phase1b_ff_arrival = model.compute_clock_arrivals(tree, false);
phase1b_timing = build_phase1b_timing_cache(...);
```

The cache includes:

```text
merged path timing
groups by FF pair
FF-to-group index
SS violation multiset
FF violation multiset
cached TNS / WNS / violation counts
```

## 8.3 Attempt limit

```cpp
const int max_phase1b_attempts = 20000;
```

## 8.4 Per-attempt time check

If time is up:

```text
early_stopped = true
message = "Optimization stopped early due to time limit in Phase 1B."
break
```

## 8.5 Stop if timing is clean

If cached SS and FF TNS are both zero:

```cpp
if (phase1b_timing.timing.ss.tns == 0.0 &&
    phase1b_timing.timing.ff.tns == 0.0) break;
```

## 8.6 Select worst target

```cpp
WorstPathChoice choice = choose_worst_target(phase1b_timing.timing, blacklist);
```

If no valid target exists:

```text
Phase 1B stopped: no non-blacklisted violating target found.
```

## 8.7 Validate target

The chosen target must exist and have a parent.

If invalid:

```text
blacklist.insert(target)
continue
```

## 8.8 Choose adaptive buffer

Required delay:

```cpp
required_delay = -choice.violation_slack;
```

Try adaptive buffer:

```cpp
adaptive_buffer = choose_batch_buffer(required_delay);
```

If no buffer fits:

```cpp
adaptive_buffer = smallest_buffer;
```

## 8.9 Insert trial buffer

The optimizer inserts a new buffer above the selected target:

```text
parent -> target
```

becomes:

```text
parent -> NEW_BUF_X -> target
```

If insertion fails:

```text
blacklist.insert(target)
continue
```

## 8.10 Incremental timing verification

After insertion:

1. Find the inserted buffer node.
2. Update SS arrivals for the inserted buffer subtree.
3. Update FF arrivals for the inserted buffer subtree.
4. Find affected path groups from changed FF names.
5. Save previous timing summary.
6. Recompute affected path groups.
7. Refresh cached TNS/WNS.

## 8.11 Phase 1B acceptance rule

The insertion is accepted only if:

```text
All four timing metrics do not degrade.
At least one of the four timing metrics improves.
```

Detailed rule:

```cpp
all_not_worse =
    new.ss.wns >= old.ss.wns - eps &&
    new.ff.wns >= old.ff.wns - eps &&
    new.ss.tns >= old.ss.tns - eps &&
    new.ff.tns >= old.ff.tns - eps;

any_improved =
    new.ss.wns > old.ss.wns + eps ||
    new.ff.wns > old.ff.wns + eps ||
    new.ss.tns > old.ss.tns + eps ||
    new.ff.tns > old.ff.tns + eps;

accept = all_not_worse && any_improved;
```

This is stricter than Phase 1A.

## 8.12 Accepted insertion

If accepted:

```cpp
summary.phase1b_insertions += 1;
summary.applied_moves.push_back(...);
```

Move string includes whether the target came from a setup or hold violation.

When `cfg.enable_timing_cache_verify` is true, the accepted cache state is immediately compared with a full timing recomputation.

## 8.13 Rejected insertion rollback

If rejected:

1. Roll back affected path groups.
2. Roll back SS arrivals.
3. Roll back FF arrivals.
4. Restore cached timing summary.
5. Remove the inserted buffer.
6. Add target to blacklist.

Important:

```text
The blacklist is target-node based.
```

Once a target fails, Phase 1B will not retry it even if later context changes.

## 8.14 End of Phase 1B

After Phase 1B stops, the optimizer captures:

```cpp
summary.after_phase1b
```

with full timing, area, legality, and runtime.

---

# 9. Phase 2 — Multi-Pass Pattern-Based Area Recovery

## 9.1 Goal

Phase 2 attempts to reduce clock-tree area after timing repair.

The current `optimizer.cpp` Phase 2 is significantly more complex than the old Strategy 2 description.

Strategy 2 described Phase 2 as:

```text
top-down removal / downsize of non-original inserted buffers
```

Current Strategy 4 Phase 2 is:

```text
multi-pass pattern matching over all buffer nodes
```

Current Phase 2 pattern sequence:

```cpp
ParallelMerge
CascadedCollapse
SizeSwap
Rebalance
CascadedCollapse
GreedyFallback
```

## 9.2 Phase 2 scans original buffers too

Current Phase 2 traversal uses:

```cpp
preorder = collect_preorder_names(tree.root.get());
```

Then it filters only by:

```cpp
is_buffer_node(node)
```

It does **not** skip original buffers at the top-level traversal.

Therefore:

```text
original buffers may be considered for type change / downsize.
original buffers are still protected from structural removal.
```

This differs from `strategy2.md`, which described Phase 2 as processing only non-original buffer nodes.

## 9.3 Phase 2 incremental timing cache

At the start of Phase 2:

```cpp
phase2_ss_arrival = model.compute_clock_arrivals(tree, true);
phase2_ff_arrival = model.compute_clock_arrivals(tree, false);
phase2_timing = build_phase1b_timing_cache(...);
current_timing = phase2_timing.timing;
```

Even though the cache type is named `Phase1bTimingCache`, it is reused here.

## 9.4 Phase 2 pattern loop

For each pattern in `pattern_sequence`:

```text
made_changes = true
while made_changes:
    made_changes = false
    take preorder snapshot
    scan all nodes
    apply this pattern where possible
```

This means each pattern repeats until it can no longer make changes.

After each pattern finishes, a Phase 2 iteration snapshot is captured.

When `cfg.enable_timing_cache_verify` is true, every accepted Phase 2 move and each pattern snapshot is checked against a full timing recomputation.

## 9.5 Phase 2 time checks

Phase 2 checks time:

- before starting each repeated pattern pass,
- before processing each node.

If time is up:

```text
early_stopped = true
message = "Optimization stopped early due to time limit in Phase 2."
```

---

# 10. Phase 2 Common Trial Helper — `try_change_types`

## 10.1 Purpose

`try_change_types` tries one or more buffer type changes atomically.

Example:

```cpp
try_change_types({
    {"BUF_A", "REALBUF_X4"},
    {"BUF_B", "REALBUF_X8"}
});
```

It is used by:

- CascadedCollapse downsize
- SizeSwap
- Rebalance
- GreedyFallback downsize

## 10.2 Validation

For each requested change:

1. Find the node.
2. Check it is a buffer node.
3. Save the old type.
4. Track the highest changed node by tree depth.

If any node is invalid:

```text
restore all saved old types
return false
```

## 10.3 Apply changes

The helper calls:

```cpp
tree.set_buffer_type(node_name, new_type)
```

for every requested change.

## 10.4 Incremental timing update

It updates arrival maps from the highest changed node.

Reason:

```text
Changing an upper buffer can affect all descendants.
Changing a lower buffer only affects its subtree.
For multiple changes, updating from the highest changed node covers all affected descendants.
```

Then it recomputes affected path groups and updates cached TNS/WNS.

## 10.5 Acceptance rule

Current rule:

```cpp
timing_not_worse(phase2_timing.timing, previous_timing)
```

Meaning:

```text
SS TNS cannot degrade.
FF TNS cannot degrade.
SS WNS cannot degrade.
FF WNS cannot degrade.
```

Important current caveat:

```text
try_change_types does not check whether area actually improves.
```

This means Phase 2 can accept type changes that preserve timing but do not reduce area, depending on the calling pattern.

## 10.6 Rejected change rollback

If rejected:

1. Roll back path groups.
2. Roll back SS arrivals.
3. Roll back FF arrivals.
4. Restore all old buffer types.
5. Restore cached timing.
6. Return false.

## 10.7 Accepted change

If accepted:

1. Cached timing becomes the new current timing.
2. Type changes remain in the tree.
3. Return true.

The caller is responsible for updating counters and move logs.

---

# 11. Phase 2 Common Trial Helper — `try_full_removal`

## 11.1 Purpose

`try_full_removal` tries to remove one buffer node and reconnect its children to its parent.

It is used by:

- CascadedCollapse
- GreedyFallback

## 11.2 Structural guard

The target cannot be removed if:

```cpp
node == nullptr
node->original == true
node->parent == nullptr
```

Therefore:

```text
Original input buffers are never structurally removed.
```

## 11.3 Removal operation

It calls:

```cpp
remove_buffer_node(tree, target_name, removed_state)
```

This removes the buffer and moves its children to the parent.

## 11.4 Incremental timing update

The affected subtree starts from the parent of the removed node.

Reason:

```text
Removing a child changes the parent's fanout.
The parent's delay may change.
All descendants under that parent may therefore be affected.
```

Then the helper recomputes affected path groups.

## 11.5 Acceptance rule

Same strict timing-preserving rule:

```cpp
timing_not_worse(new_timing, previous_timing)
```

No explicit area check is needed for removal because removing a non-original buffer always decreases area if the removed type has positive area.

## 11.6 Accepted removal

If accepted:

```cpp
summary.phase2_removals += 1;
summary.applied_moves.push_back("Phase2 remove " + target_name);
```

The caller's removed counter is also incremented.

## 11.7 Rejected removal rollback

If rejected:

1. Roll back path groups.
2. Roll back arrival maps.
3. Restore cached timing.
4. Restore removed buffer node using `restore_buffer_node`.
5. Return false.

---

# 12. Phase 2 Pattern: `ParallelMerge`

## 12.1 Goal

Merge sibling buffers with the same type under the same parent.

Before:

```text
parent
  ├─ BUF_A(type X)
  │    └─ subtree A
  ├─ BUF_B(type X)
  │    └─ subtree B
  └─ BUF_C(type X)
       └─ subtree C
```

After:

```text
parent
  └─ survivor(type X)
       ├─ subtree A
       ├─ subtree B
       └─ subtree C
```

This can reduce area by removing duplicate sibling buffers.

## 12.2 Candidate selection

For the current node:

1. Get its parent.
2. Scan all children of the parent.
3. Collect sibling buffer nodes with the same type as current node.
4. If fewer than 2 same-type siblings exist, skip.

## 12.3 Survivor choice

Default survivor:

```text
first same-type sibling
```

If an original sibling exists:

```text
prefer the original sibling as survivor
```

Reason:

```text
Original nodes cannot be removed, so keeping an original node avoids illegal deletion.
```

## 12.4 Removal restrictions

The merge removes all same-type siblings except survivor.

If any node that would be removed is original:

```text
cancel this merge attempt
```

Therefore:

```text
ParallelMerge may keep an original buffer, but does not remove original buffers.
```

## 12.5 Fanout check

The merged fanout is:

```text
survivor.children.size()
+ sum(children.size() of removed siblings)
```

If merged fanout exceeds survivor library support:

```text
skip this merge
```

Current check uses the SS delay table length.

## 12.6 Apply merge

`apply_parallel_merge`:

1. Removes selected sibling nodes from the parent.
2. Moves their children into the survivor.
3. Updates parent pointers.
4. Removes deleted nodes from the tree index.
5. Stores rollback information.

## 12.7 Timing verification

Since merging changes parent fanout and survivor fanout, the affected update starts from the parent.

Then affected path groups are recomputed.

## 12.8 Acceptance

Accept if:

```cpp
timing_not_worse(new_timing, previous_timing)
```

If accepted:

```cpp
summary.phase2_removals += number_of_removed_siblings;
summary.applied_moves.push_back("Phase2 parallel-merge at P keep S");
```

## 12.9 Rejection rollback

If rejected:

1. Roll back path groups.
2. Roll back SS/FF arrivals.
3. Restore previous cached timing.
4. Roll back the structural merge.

---

# 13. Phase 2 Pattern: `CascadedCollapse`

## 13.1 Goal

Simplify two cascaded buffers on a single chain.

Pattern:

```text
node -> child -> subtree
```

where both `node` and `child` are buffer nodes.

Precondition:

```cpp
node->children.size() == 1
child is buffer node
```

The pattern tries several actions in order.

## 13.2 Action 1: remove upper node

Try:

```cpp
try_full_removal(node_name)
```

If accepted:

```text
made_changes = true
continue to next scanned node
```

## 13.3 Action 2: remove lower child

If removing the upper node fails, re-find node and child, then try:

```cpp
try_full_removal(child_name)
```

If accepted:

```text
made_changes = true
continue
```

## 13.4 Action 3: downsize both node and child

If neither removal succeeds:

```cpp
first_downsize  = choose_next_smaller_buffer(node->type, node->children.size())
second_downsize = choose_next_smaller_buffer(child->type, child->children.size())
```

If both exist:

```cpp
try_change_types({
    {node_name, first_downsize->name},
    {child_name, second_downsize->name}
})
```

If accepted:

```cpp
summary.phase2_downsizes += 2;
```

## 13.5 Action 4: downsize upper node only

After refreshing node / child pointers:

```cpp
try_change_types({{node->name, first_downsize->name}})
```

If accepted:

```cpp
summary.phase2_downsizes += 1;
```

## 13.6 Action 5: downsize lower child only

After refreshing node / child pointers again:

```cpp
try_change_types({{child->name, second_downsize->name}})
```

If accepted:

```cpp
summary.phase2_downsizes += 1;
```

## 13.7 Why refresh pointers repeatedly?

A previous action may:

- remove a node,
- change tree structure,
- invalidate old pointers,
- alter the current node's child relationship.

Therefore the implementation repeatedly calls `tree.find_node(...)` after each trial.

---

# 14. Phase 2 Pattern: `SizeSwap`

## 14.1 Goal

Swap the types of two cascaded buffers when the upper one is physically smaller than the lower one.

Pattern:

```text
node(type small) -> child(type large)
```

Trial:

```text
node(type large) -> child(type small)
```

## 14.2 Preconditions

```cpp
node->children.size() == 1
child is buffer node
first_lib exists
second_lib exists
is_physically_smaller(first_lib, second_lib)
```

The last condition means:

```text
upper buffer is smaller than lower buffer
```

## 14.3 Fanout compatibility

Before swap:

```text
node fanout must be supported by child old type
child fanout must be supported by node old type
```

Current check uses SS delay table size.

## 14.4 Trial

```cpp
try_change_types({
    {node_name, child_old_type},
    {child_name, node_old_type}
})
```

## 14.5 Acceptance

Accepted if timing does not worsen.

## 14.6 Important caveat

`SizeSwap` does not necessarily reduce area.

It swaps two types, so total area is usually unchanged.

However, current implementation records:

```cpp
summary.phase2_downsizes += 2;
```

This means the downsize counter is not semantically exact for this pattern.

Potential rationale:

```text
SizeSwap may enable later CascadedCollapse or downsize moves.
```

But current implementation does not explicitly require that enabling benefit to occur.

---

# 15. Phase 2 Pattern: `Rebalance`

## 15.1 Goal

Replace an imbalanced cascaded buffer pair with two medium-sized buffers.

Pattern:

```text
node(type very small or very large) -> child(type very large or very small)
```

Trial:

```text
node(type medium) -> child(type medium)
```

## 15.2 Preconditions

```cpp
node->children.size() == 1
child is buffer node
both library entries exist
```

Compute:

```cpp
smaller_area = min(first_area, second_area)
larger_area  = max(first_area, second_area)
```

Skip if:

```cpp
smaller_area <= 0.0
larger_area / smaller_area < phase2_gap_threshold
```

Current threshold:

```cpp
phase2_gap_threshold = 2.5
```

So rebalance only triggers when one buffer is at least 2.5x larger than the other.

## 15.3 Medium buffer selection

```cpp
medium = choose_medium_buffer(libs, model, node_fanout, child_fanout)
```

The medium buffer is chosen near the median area among cells supporting both fanouts.

## 15.4 Compatibility checks

Skip if:

```text
medium does not exist
medium is already both node type and child type
medium does not support either fanout
```

Current fanout support check uses SS delay table length.

## 15.5 Trial

```cpp
try_change_types({
    {node_name, medium->name},
    {child_name, medium->name}
})
```

## 15.6 Acceptance

Accepted if timing does not worsen.

## 15.7 Important caveat

Current `Rebalance` does not explicitly check whether total area decreases.

For example:

```text
large + small -> medium + medium
```

may or may not reduce area depending on the library.

Current implementation still counts accepted rebalance as:

```cpp
summary.phase2_downsizes += 2;
```

This should be treated as a known QoR / reporting risk.

---

# 16. Phase 2 Pattern: second `CascadedCollapse`

The pattern sequence contains `CascadedCollapse` twice:

```text
ParallelMerge
CascadedCollapse
SizeSwap
Rebalance
CascadedCollapse
GreedyFallback
```

Reason:

```text
SizeSwap and Rebalance can modify cascaded buffer type relationships.
After those changes, a cascaded pair that was previously not removable or not downsizeable may become removable or downsizeable.
```

Therefore, the optimizer runs `CascadedCollapse` again before the final greedy fallback.

---

# 17. Phase 2 Pattern: `GreedyFallback`

## 17.1 Goal

Perform simple conservative cleanup after all special patterns.

For each buffer node:

1. If it is not original, try full removal.
2. If removal is not possible or not accepted, try one-step downsize.

## 17.2 Removal trial

Only for non-original nodes:

```cpp
if (!node->original) {
    try_full_removal(node_name)
}
```

Original nodes are skipped for removal.

## 17.3 Downsize trial

For both original and non-original buffers:

```cpp
smaller = choose_next_smaller_buffer(libs, model, node->type, node->children.size())
```

If a smaller type exists:

```cpp
try_change_types({{node->name, smaller->name}})
```

If accepted:

```cpp
summary.phase2_downsizes += 1;
summary.applied_moves.push_back("Phase2 greedy-downsize ...");
```

## 17.4 Important difference from Strategy 2

Strategy 2 described Phase 2 as processing only non-original inserted buffers.

Current implementation allows original buffers to be downsized / type-changed in GreedyFallback.

This is legal as long as:

```text
original node name is preserved
original node is not deleted
only the cell type changes
```

---

# 18. Finalization

After all phases:

## 18.1 Full final timing

```cpp
summary.final_timing = model.analyze_timing(tree, ss_paths, ff_paths, clock_period);
```

This does not rely on the incremental cache.

## 18.2 Final area

```cpp
summary.final_area = model.compute_tree_area(tree);
```

## 18.3 Final legality

```cpp
summary.final_legality = model.validate_legality(tree, libs);
```

## 18.4 Final score

```cpp
summary.final_score = model.compute_score_metrics(
    summary.final_timing,
    baseline.timing,
    summary.final_area,
    baseline.area,
    cfg.alpha,
    cfg.beta,
    cfg.gamma
);
```

Important caveat:

```text
OptimizerConfig currently defaults alpha = beta = gamma = 1.0.
This is not the official hidden weighting.
```

The final score is useful for internal comparison, but should not be treated as the exact contest score.

## 18.5 Success flag

```cpp
summary.success = summary.final_legality.ok;
```

The optimizer does not require final timing to be clean for `success`; it requires output legality.

## 18.6 Final message

If no early stop and legality is OK:

```text
Optimization completed successfully.
```

If no early stop and legality has issues:

```text
Optimization completed with legality issues.
```

If early stopped and no message exists:

```text
Optimization stopped early.
```

---

# 19. Current Strategy 4 vs Strategy 2

## 19.1 Same as Strategy 2

Strategy 4 keeps these Strategy 2 ideas:

```text
Phase 1A coarse batched fixing
Phase 1B fine greedy cleanup
Phase 1B incremental timing cache
strict TNS/WNS non-degradation in fine trials
local rollback for rejected trials
global 570-second safety limit
stage snapshots
final full timing / area / legality recomputation
```

## 19.2 Different from Strategy 2

Major differences:

| Topic | Strategy 2 | Current optimizer.cpp / Strategy 4 |
|---|---|---|
| Phase 2 scope | non-original buffers only | scans all buffer nodes |
| Original buffer type change | not emphasized | allowed in Phase 2 downsize/type-change |
| Original buffer removal | not allowed | still not allowed |
| Phase 2 style | top-down removal/downsize | multi-pass pattern matching |
| Phase 2 patterns | removal + downsize | ParallelMerge, CascadedCollapse, SizeSwap, Rebalance, CascadedCollapse, GreedyFallback |
| Phase 2 timing eval | described like oracle calls | mostly incremental cache |
| SizeSwap | absent | present |
| Rebalance | absent | present |
| Phase2 iteration snapshots | less detailed | one snapshot per pattern pass |

---

# 20. Known Risks / Known Half-Finished Areas

This section documents issues that are important for future work.

## 20.1 Phase 1A only checks TNS

Current Phase 1A rejection rule checks:

```text
SS TNS and FF TNS
```

It does not check:

```text
SS WNS or FF WNS
```

Risk:

```text
A batch may improve TNS but worsen WNS.
```

Recommended conservative fix:

```text
Use timing_not_worse(...) for Phase 1A batch acceptance.
```

Possible aggressive alternative:

```text
Allow small WNS degradation only if TNS improvement is large.
```

But that should be tested carefully.

## 20.2 Phase 1A plateau condition ignores zero improvement

Current stop condition:

```cpp
tns_improvement > 0.0 && tns_improvement < improvement_threshold
```

If improvement is exactly zero, Phase 1A does not plateau-stop.

Recommended fix:

```cpp
tns_improvement >= 0.0 && tns_improvement < improvement_threshold
```

or simply:

```text
stop if improvement <= eps
```

## 20.3 `phase1a_broke_due_to_entanglement` is unused

This variable may be set when batch target construction fails or insertion cannot progress.

Current issue:

```text
It does not affect later behavior.
```

Recommended options:

1. Remove it if unnecessary.
2. Use it to print / record why Phase 1A handed off to Phase 1B.
3. Use it to change Phase 1B blacklist behavior.

## 20.4 Phase 2 type-change acceptance does not check area

Current rule:

```text
accept type change if timing does not worsen
```

Missing:

```text
new area < old area
```

This affects:

- SizeSwap
- Rebalance
- possibly any future multi-type trial

Recommended fix:

```text
For Phase 2 area recovery, accept only if timing_not_worse and area improves.
```

Exception:

```text
If a type change is intentionally used as an enabling move, document it separately and verify it enables a later area gain.
```

## 20.5 `SizeSwap` and `Rebalance` counters are misleading

Current implementation increments:

```cpp
phase2_downsizes += 2
```

for SizeSwap and Rebalance.

But:

```text
SizeSwap is a swap, not a downsize.
Rebalance may or may not reduce area.
```

Recommended fix:

```text
Separate counters:
- phase2_removals
- phase2_downsizes
- phase2_swaps
- phase2_rebalances
```

or only count as downsize if area decreases.

## 20.6 Fanout compatibility checks often use SS delay size only

Several helpers check candidate fanout using:

```cpp
fanout <= candidate.ss_delay.size()
```

Recommended fix:

```cpp
fanout <= candidate.ss_delay.size()
&& fanout <= candidate.ff_delay.size()
```

This is safer if SS and FF delay tables ever differ in length.

## 20.7 Phase 1B blacklist is target-level and permanent

If one insertion trial fails for a target:

```text
target is blacklisted for the rest of Phase 1B
```

Risk:

```text
Later accepted moves may change context, but the target is still skipped.
```

Possible future improvement:

```text
Use a retry budget or clear blacklist after a successful move.
```

## 20.8 No Phase 1 existing-buffer resize repair yet

The current Phase 1B fixes timing only by inserting new buffers.

A high-value future improvement is:

```text
Before inserting NEW_BUF, try changing existing buffer type along the relevant launch/capture branch.
```

For example:

```text
setup violation: try increasing capture-side delay through a smaller/slower buffer type
hold violation : try increasing launch-side delay through a smaller/slower buffer type
```

This could improve timing without adding area, and sometimes even reduce area.

---

# 21. Recommended Near-Term Patch Plan

This section is not current implementation. It is a practical next-step plan for making Strategy 4 safer.

## Completed safety patches

The current implementation already includes:

```text
- full timing-cache verification after accepted Phase 1B moves
- full timing-cache verification after accepted Phase 2 moves
- full timing-cache verification at stage and iteration snapshots
- transactional Phase 1A summary accounting
```

These checks are controlled from `OptimizerConfig`, mainly through `enable_timing_cache_verify`, `enable_phase0_incremental_verify`, and `enable_fast_timing_verify`. A timing mismatch greater than `1e-6`, or any violation-count mismatch in the full cache verifier, prints cached/full values with the move or snapshot context and aborts.

## Patch 1: Strengthen Phase 1A acceptance

Option A, conservative:

```text
Use timing_not_worse for batch acceptance.
```

Option B, balanced:

```text
Require TNS non-degradation and WNS non-degradation within a small tolerance.
```

## Patch 2: Add area guard to Phase 2 type changes

For Phase 2:

```text
accept type change only if timing_not_worse and area improves
```

This will make Phase 2 truly area-recovery oriented.

## Patch 3: Add equivalent-delay downsizing

Before normal downsize, try:

```text
same SS/FF delay for current fanout
smaller area
```

This is low risk because timing should remain almost unchanged.

## Patch 4: Add Phase 1B resize-before-insert

Before inserting a new buffer for the chosen target:

```text
try existing-buffer type change on the relevant branch
if accepted, skip insertion
otherwise fall back to current insertion path
```

Start with conservative candidates only:

```text
area non-increasing
delay direction helps the selected violation
TNS/WNS do not degrade
at least one timing metric improves or area decreases
```

---

# 22. One-Sentence Summary

Strategy 6 is the current optimizer implementation: **default non-reset Phase 0 resizes existing buffers using FastTimingEngine trials, then the optimizer runs repeated Phase1A-pulse / pressure-guided reclaim cycles until the configured cycle window ends, then Final Alternating Greedy performs the default final repair/reclaim cleanup with fast reclaim trials.**

---

# 23. Debug Output and Timing Report

`ENABLE_OUTPUT` controls debug output and report generation:

```bash
cmake .. -DENABLE_OUTPUT=ON
```

When enabled, the program:

- prints optimizer progress and summaries
- writes `timing_report_<testcase>.txt` beside the requested output tree
- does not run expensive full timing-cache verification by itself

When disabled, none of those debug or report operations run. Expensive full timing-cache and fast-timing verification is controlled separately from `OptimizerConfig` in `src/optimizer.h`, not from CMake. Relevant flags include `enable_timing_cache_verify`, `enable_phase0_incremental_verify`, `enable_fast_timing_verify`, `enable_phase0_trial_full_validation_verify`, and `enable_final_alt_trial_full_validation_verify`.

The report contains:

- run metadata, constraints, status, legality, violations, modifications, and score
- a compact Baseline / After Phase0 / RepairReclaimCycles / FinalAlternatingGreedy table when cycle mode is enabled
- a detailed Repair/Reclaim cycle summary and per-cycle table
- runtime / validation optimization diagnostics
- the Phase1A stop reason
- a Phase 1A iteration table with cumulative insertion counts
- a Phase 2 pattern table with per-pass removal and resize counts when legacy Phase2 runs

Its timing columns are SS/FF TNS, WNS, and violation counts. It does not include delta-TNS or delta-area columns.

---

# 24. Phase 0 and Reset Experiment

Non-reset Phase 0 is part of the default output-producing flow:

```text
original tree -> Phase0 -> Repair/Reclaim cycles -> FinalAlternatingGreedy
```

The legacy final Phase1B plus Phase2 cleanup can still be selected by setting either `enable_final_alternating_greedy = false` or `final_alt_replace_phase1b_and_phase2 = false` in `OptimizerConfig`.

The optional reset-based comparison experiment is enabled by setting `enable_phase0_reset_experiment = true` in `OptimizerConfig`.

The normal non-reset flow still produces the contest output. When the experiment is enabled, the parsed tree is also cloned for an independent comparison branch:

```text
normal                  : original tree -> Phase0 -> Repair/Reclaim cycles -> FinalAlternatingGreedy
reset_phase0_experiment : fastest-compatible reset -> Phase0 -> Repair/Reclaim cycles -> FinalAlternatingGreedy
```

The reset type is the minimum-average-delay library cell that supports the maximum existing buffer fanout in both SS and FF tables. Ties prefer smaller area.

Phase 0 only changes existing buffer types. It never inserts, removes, or renames nodes. Its normalized objective is:

```text
|SS_TNS| / baseline |SS_TNS|
+ |FF_TNS| / baseline |FF_TNS|
+ WNS_WEIGHT * (
    |SS_WNS| / baseline |SS_WNS|
  + |FF_WNS| / baseline |FF_WNS|
  )
```

Normalization denominators are clamped to `1e-9`. The default `WNS_WEIGHT` is `0.5`.

Candidate pressure is computed from violating paths by walking launch/capture ancestors to their LCA:

- setup: positive on the capture-only branch, negative on the launch-only branch
- hold: positive on the launch-only branch, negative on the capture-only branch

Positive pressure tries slower compatible cells; negative pressure tries faster compatible cells. Zero-pressure nodes are counted for reporting and auto-batch sizing but are not ranked for resize trials.

At most three candidate types are tested per node, for at most two passes and 90 seconds. Phase 0 uses FastTimingEngine resize trials by default. Because FastTimingEngine summaries do not carry per-path `path_results`, Phase 0 recomputes full timing only at pressure-ranking boundaries when path-level pressure is needed. The old incremental timing path remains available through `enable_fast_timing_engine=false`, and debug full-timing verification can be enabled with `enable_phase0_incremental_verify` or `enable_fast_timing_verify`.

The Phase 0 cap is also bounded by the shared 570-second global timer used by all later phases. A trial is accepted only when:

- local resize legality remains valid, with optional full validation verification
- normalized timing cost improves by more than `1e-9`
- SS and FF WNS do not degrade by more than `1e-6`

Area is a ranking and tie-break metric, not an acceptance constraint.

Phase 0 scans ranked candidates in `unlimited_by_count` mode by default (`phase0_node_fraction=1.0`, `phase0_max_trial_nodes=0`). The pass is still bounded by the Phase 0 time budget, global optimizer budget, max passes, and low-value tail early-stop counters.

Batch trial is manual-or-auto: `enable_phase0_batch_trial` forces it on, while `enable_phase0_batch_auto` enables it automatically for large cases. The default auto thresholds are SS violations `>=15000` or pressure candidates `>=10000`; otherwise small and medium cases use incremental single-trial mode. Batch size stays `8`, and split-on-fail stays enabled.

The normal report includes Baseline, After Phase0, RepairReclaimCycles, and FinalAlternatingGreedy by default. Disabling Final Alternating Greedy replacement in `OptimizerConfig` restores separate Phase1B and Phase2 rows. It records Phase 0 settings/counters, candidate count mode, ranked/scanned candidates, early-stop reason, timing mode, verification status, average trial time, batch auto reason/counters, Repair/Reclaim cycle counters, FastTimingEngine diagnostics, runtime/validation optimization diagnostics, and the Phase1A/cycle stop reason. The reset experiment additionally writes `reset_phase0_experiment` and `phase0_compare` reports.

---

# 25. Strategy 6 Repair/Reclaim Cycle Stage

Strategy 6 keeps the Strategy 5 post-Phase0 architecture and accelerates its hot trial loops:

```text
Phase0
-> repeated Repair/Reclaim cycles until 480 seconds by default
-> Final Alternating Greedy final cleanup by default
```

The Repair/Reclaim cycle stage can be disabled by setting:

```cpp
cfg.enable_repair_reclaim_cycles = false;
```

The old final Phase1B plus Phase2 cleanup is still available by disabling Final Alternating Greedy replacement in `OptimizerConfig`.

Default cycle configuration:

```cpp
enable_repair_reclaim_cycles = true;
repair_reclaim_cycle_end_time_seconds = 480.0;
repair_reclaim_max_cycles = 100;
phase1a_pulse_delay_scale = 1.0;
enable_pressure_guided_full_tree_reclaim = true;
cycle_reclaim_time_budget_seconds = 210.0;
reclaim_giveback_ratio = 0.20;
reclaim_max_consecutive_rejects = 500;
reclaim_max_trials_per_cycle = 0;
reclaim_allow_original_resize = true;
reclaim_allow_newbuf_resize = true;
reclaim_allow_newbuf_delete = true;
reclaim_allow_area_increasing_moves = false;
enable_adaptive_repair_reclaim_params = true;
adaptive_reclaim_giveback_ratio = 0.20;
small_case_phase1a_pulse_delay_scale = 0.5;
medium_case_phase1a_pulse_delay_scale = 1.0;
large_case_phase1a_pulse_delay_scale = 1.5;
enable_repair_reclaim_no_progress_stop = true;
repair_reclaim_no_progress_streak_limit = 20;
repair_reclaim_min_timing_cost_improvement = 1e-6;
repair_reclaim_min_area_improvement = 1e-6;
repair_reclaim_no_progress_max_insertions = 1;
enable_incremental_area_verify = false;
enable_phase0_trial_full_validation_verify = false;
enable_final_alt_trial_full_validation_verify = false;
```

When adaptive mode is enabled, the manual `phase1a_pulse_delay_scale` and `reclaim_giveback_ratio` fields are treated as manual fallback/reporting values. The effective Repair/Reclaim parameters are selected after Phase0 from baseline SS violations, Phase0 pressure/ranked candidate count, and Phase0 batch-mode usage:

- large: SS violations `>=30000` or pressure candidates `>=20000`, scale `1.5`
- medium: SS violations `>=15000`, pressure candidates `>=10000`, or Phase0 batch mode used, scale `1.0`
- small: otherwise, scale `0.5`

All adaptive classes use selected giveback ratio `0.20` by default. Set `enable_adaptive_repair_reclaim_params=false` to recover exact manual behavior.

## 25.1 Phase1A-Pulse

Each cycle starts with one Phase1A-pulse. It reuses the existing Phase1A target ordering and independent endpoint batching logic, but stops after exactly one accepted batch attempt.

For each target, the existing required delay `d` is scaled before buffer selection:

```text
scaled_required_delay = phase1a_pulse_delay_scale * d
```

The default scale is `1.0`. Later experiments can sweep `1.0`, `0.75`, and `0.5`.

The pulse keeps Phase1A's current batch acceptance behavior:

- accept only if both SS and FF TNS do not degrade
- rollback all trial insertions if the batch worsens either corner TNS
- commit insertion counters and move strings only after acceptance

## 25.2 Pressure-Guided Full-Tree Reclaim

After an accepted pulse, the optimizer recomputes pressure on the full current tree using endpoint accumulation:

```text
setup violation:
  endpoint_delta[capture_ff] += -setup_slack
  endpoint_delta[launch_ff]  -= -setup_slack

hold violation:
  endpoint_delta[launch_ff]  += -hold_slack
  endpoint_delta[capture_ff] -= -hold_slack
```

Then a single postorder traversal accumulates subtree pressure:

```text
pressure[node] = endpoint_delta[node] + sum(pressure[child])
```

This pressure ranks reclaim candidates only. Actual acceptance still uses incremental timing evaluation and legality checks.

Generated reclaim candidates:

- original buffers: area-reducing resize only
- NEW_BUF buffers: area-reducing resize and delete
- no original deletes or renames
- no area-increasing moves by default

Candidates are ranked by area saving first, then timing pressure effect, with a small bonus for NEW_BUF deletes.

## 25.3 Giveback Budget

Before the pulse, timing is recorded. After the pulse, timing gain is measured:

```text
ss_tns_gain = max(0, abs(before_ss_tns) - abs(after_ss_tns))
ff_tns_gain = max(0, abs(before_ff_tns) - abs(after_ff_tns))
```

Reclaim may give back only a configured fraction of that gain:

```text
allowed_ss_tns_giveback = reclaim_giveback_ratio * ss_tns_gain
allowed_ff_tns_giveback = reclaim_giveback_ratio * ff_tns_gain
```

The first implementation also requires:

- SS and FF WNS degrade by no more than `0.001`
- violation counts increase by no more than 5% of the post-pulse count
- final legality remains valid
- area decreases unless `reclaim_allow_area_increasing_moves` is enabled

## 25.4 Reclaim Trial Mechanics

Reclaim uses the existing Phase1B/Phase2 incremental timing infrastructure:

- compute SS/FF arrivals
- build `Phase1bTimingCache`
- apply candidate
- update affected subtree arrivals
- update affected path groups
- accept if legality, area, and giveback checks pass
- otherwise rollback tree connectivity/type, arrivals, and path-group timing

Deletion is restricted to non-original `NEW_BUF` nodes and uses the existing remove/restore buffer helpers.

## 25.5 Stop Conditions

Per-cycle reclaim stops on:

- `cycle_reclaim_time_budget_seconds`
- reaching the global cycle window
- reaching the total optimizer time limit
- candidates exhausted
- `reclaim_max_consecutive_rejects`
- `reclaim_max_trials_per_cycle` when greater than zero

The repeated cycle stage stops on:

- elapsed runtime reaching `repair_reclaim_cycle_end_time_seconds`
- `repair_reclaim_max_cycles`
- timing already closed
- Phase1A-pulse inserted zero accepted buffers
- total optimizer time limit
- `no_progress_streak` when conservative no-progress detection reaches `repair_reclaim_no_progress_streak_limit`

After that, Final Alternating Greedy runs as the default final repair/reclaim stage. If Final Alternating Greedy replacement is disabled in `OptimizerConfig`, the legacy Phase1B and Phase2 stages run instead.

## 25.6 Reporting

The timing report now includes:

- `RuntimeProfile`, a compact phase/substage timing breakdown when `enable_runtime_profiling` is true
- `Runtime / Validation Optimization Diagnostics`, showing local validation counts, full validation counts, full area recomputations, incremental area updates, library cache hits/misses, and Repair/Reclaim no-progress stop status
- `Adaptive Repair/Reclaim Parameters`, showing enabled/manual mode, classification signals, selected case size, and selected/manual values
- total cycles
- total pulse insertions
- original resizes accepted during reclaim
- NEW_BUF resizes accepted during reclaim
- NEW_BUF deletes accepted during reclaim
- total pulse area increase
- total reclaim area saved
- total net cycle area delta
- average and weighted reclaim coverage
- total reclaim runtime, candidate count, trial count, accepted count, and accept rate
- area saved by original resize, NEW_BUF resize, and NEW_BUF delete
- cycle-stage stop reason
- per-cycle pulse gain, candidates, accepted/rejected trials, giveback used, final timing, and area

The detailed cycle report is split into compact tables:

- **Area Balance**: `AreaBefore`, `AreaPulse`, `AreaReclaim`, `PulseArea+`, `ReclaimSave`, `ReclaimCov`, and `NetArea`
- **Timing Giveback**: SS/FF TNS gain, allowed giveback, used giveback, utilization, WNS worsening, and final timing
- **Reclaim Efficiency**: runtime, candidates, tried/accepted/rejected, accept rate, trials per second, area saved per second, area saved per accepted move, and category area saved

Report-only diagnostics are printed when reclaim coverage falls below 50%, a cycle increases net area, or reclaim accept rate is below 5%.

Runtime profiling is controlled by:

```cpp
enable_runtime_profiling = true;
```

The profile line reports Phase0, Repair/Reclaim, Phase1A-pulse, pressure computation, reclaim candidate generation/sorting, reclaim trial loop, legacy Phase1B/Phase2 fields when used, final validation, and report generation time.

Phase0, Repair/Reclaim, and Final Alternating Greedy trial loops avoid full-tree area recomputation and full-tree legality validation per candidate. Instead they use current local area deltas plus conservative local legality checks, while keeping stage snapshots and final full legality validation unchanged. Optional debug flags can re-enable full validation/area verification checks after accepted moves.

## 25.7 Final Alternating Greedy

Strategy 6 uses Final Alternating Greedy as the default final-stage cleanup:

```text
Phase0
-> Repair/Reclaim cycles
-> Final Alternating Greedy
```

It is enabled by default. To recover the old final Phase1B plus Phase2 cleanup, set `enable_final_alternating_greedy = false` or `final_alt_replace_phase1b_and_phase2 = false` in `OptimizerConfig`.

Final Alternating Greedy replaces the old final Phase1B plus Phase2 sequence by default. Each iteration performs:

1. a bounded Phase1B-style repair batch, inserting at most `final_alt_repair_insertions_per_iter` buffers
2. a GreedyFallback-style reclaim pass that tries NEW_BUF deletes and smaller-buffer resizes

It intentionally does not run the full Phase2 pattern sequence (`ParallelMerge`, `CascadedCollapse`, `SizeSwap`, `Rebalance`). Reclaim acceptance is guarded by a timing giveback budget, optional hard WNS guard, optional area-decrease requirement, local legality checks, and final full legality validation.

Default configuration:

```cpp
enable_final_alternating_greedy = true;
final_alt_replace_phase1b_and_phase2 = true;
final_alt_max_iters = 1000;
final_alt_repair_insertions_per_iter = 150;
final_alt_time_budget_seconds = 0.0;
final_alt_safety_margin_seconds = 0.0;
final_alt_repair_ss_tns_threshold = -0.0;
final_alt_repair_ss_wns_threshold = -0.000;
final_alt_repair_ss_violation_threshold = 0;
final_alt_repair_ff_tns_threshold = -0.00;
final_alt_repair_ff_violation_threshold = 0;
final_alt_reclaim_giveback_ratio = 0.05;
final_alt_reclaim_area_decrease_only = false;
final_alt_reclaim_hard_wns_guard = false;
final_alt_run_reclaim_when_no_repair_needed = true;
```

The timing report includes a `Final Alternating Greedy` section with settings, runtime, stop reason, repair insertions, reclaim accepts, NEW_BUF deletes, resizes, area before/after/saved, and SS/FF timing before/after. It also includes a per-iteration Final Alternating Greedy table with repair/reclaim attempts, accepts/rejections, timing deltas, area deltas, and stop reasons. In the stage contribution table, default replacement mode prints one `FinalAlternatingGreedy` row instead of separate `Phase1B` and `Phase2` rows.
