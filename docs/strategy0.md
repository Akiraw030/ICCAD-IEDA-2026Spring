# Strategy 0: Oracle-Driven Greedy CTS Baseline

## Purpose
This document summarizes the current baseline implementation of `Optimizer::optimize`.

The strategy has two phases:
- Phase 1: Iterative greedy timing fixing (buffer insertion)
- Phase 2: Top-down area recovery (remove/downsize while preserving timing quality)

The method relies on `analyze_timing` as the timing oracle and uses local undo for failed trials.

## Inputs
- `ClockTree& tree`
- `const std::vector<BufSpec>& libs`
- `const std::vector<PathInfo>& ss_paths`
- `const std::vector<PathInfo>& ff_paths`
- `double clock_period`

## Outputs (`OptimizationSummary`)
- `success`
- `message`
- `final_timing`
- `final_score`
- `final_legality`
- `final_area`
- `iterations`
- `phase1_insertions`
- `phase2_removals`
- `phase2_downsizes`
- `applied_moves`

## Global Safety Valve
- Timer starts at the beginning of `optimize`.
- Hard limit: `570.0` seconds.
- Lambda `is_time_up()` checks elapsed wall time.
- Both Phase 1 and Phase 2 loops break early if time is up.

## Initialization
1. Validate root exists.
2. Build `DelayModel` from `libs`.
3. Select one insertion buffer for Phase 1 using `choose_smallest_buffer`:
- Primary key: smallest area
- Tie-breaker: smaller nominal delay
4. Initialize `baseline` once:
- `baseline.timing = analyze_timing(...)`
- `baseline.area = compute_tree_area(...)`
5. Create `blacklist` as `std::set<std::string>`.

## Phase 1: Iterative Greedy Timing Fix
Loop: `for iter in [0, max_iterations)` where `max_iterations = 1000`.

Per iteration:
1. Time check: stop if `is_time_up()`.
2. Oracle call: `current_timing = analyze_timing(...)`.
3. If `current_timing.ss.tns == 0` and `current_timing.ff.tns == 0`, stop Phase 1.
4. Select target by worst violating path (`choose_worst_target`):
- If setup is worse: target `capture_ff`
- If hold is worse: target `launch_ff`
- Skip nodes already in blacklist
5. Trial insertion:
- Insert the same smallest buffer above target using `insert_buffer_between(parent, target, new_name, smallest_type)`.
6. Oracle verify trial (`trial_timing = analyze_timing(...)`).
7. Keep insertion only if TNS improves monotonically:
- `trial.ss.tns >= current.ss.tns`
- `trial.ff.tns >= current.ff.tns`
- and at least one is strictly greater
8. If failed:
- Undo insertion via `tree.delete_buffer(new_buffer_name)`
- Add target to blacklist

## Phase 2: Top-Down Area Recovery
- Start from `phase2_timing = analyze_timing(...)` after Phase 1.
- Traverse nodes in preorder DFS snapshot (`collect_preorder_names`).
- Only process buffer nodes that are not sinks and not original nodes.

For each node:
1. Time check: stop if `is_time_up()`.
2. Attempt Action A: removal trial
- Temporarily remove buffer via `remove_buffer_node`
- Oracle call
- Accept if non-worse relative to current phase2 baseline:
  - `trial.ss.tns >= phase2_timing.ss.tns`
  - `trial.ff.tns >= phase2_timing.ff.tns`
- Else restore with `restore_buffer_node`
3. Attempt Action B: downsizing trial (if removal not kept)
- Find next smaller candidate with `choose_next_smaller_buffer`
- Temporarily set type via `set_buffer_type`
- Oracle call
- Accept with the same non-worse rule as removal
- Else revert to old type

Note: this version intentionally does not use setup-slack prefiltering; decisions are oracle-driven.

## Finalization
After both phases:
1. `final_timing = analyze_timing(...)`
2. `final_area = compute_tree_area(...)`
3. `final_legality = validate_legality(...)`
4. `final_score = compute_score_metrics(final_timing, baseline.timing, final_area, baseline.area, alpha, beta, gamma)`
5. `success = final_legality.ok`

## Key Behavior Summary
- Timing repair in Phase 1 is aggressive and simple (single tiny buffer type + blacklist).
- Area recovery in Phase 2 is conservative (only keep changes that do not worsen SS/FF TNS baseline).
- Runtime is guarded by a strict global time limit (570 seconds).

## Known Tradeoffs
- Phase 1 uses one fixed insertion cell type, so convergence quality depends on library characteristics.
- Blacklist is node-based; it may skip revisiting a node after context changes.
- Phase 2 acceptance compares only TNS (not WNS/path count), favoring stability over fine-grained QoR.
