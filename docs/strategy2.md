# Strategy 2: Coarse-to-Fine Oracle-Driven CTS Optimization

## Purpose
This document summarizes the current implementation of `Optimizer::optimize`.

The strategy uses three stages:
- Phase 1A: coarse, batched buffer insertion on groups of violating paths
- Phase 1B: fine greedy buffer insertion with adaptive sizing
- Phase 2: top-down area recovery with strict corner-preserving acceptance

The optimizer treats `analyze_timing(...)` as the oracle and uses local undo for failed structural trials.

## Inputs
- `ClockTree& tree`
- `const std::vector<BufSpec>& libs`
- `const std::vector<PathInfo>& ss_paths`
- `const std::vector<PathInfo>& ff_paths`
- `double clock_period`

## Outputs (`OptimizationSummary`)
- `success`
- `early_stopped`
- `message`
- `final_timing`
- `final_score`
- `final_legality`
- `final_area`
- `iterations`
- `phase1a_insertions`
- `phase1b_insertions`
- `phase2_removals`
- `phase2_downsizes`
- `after_phase1a`
- `after_phase1b`
- `after_phase2`
- `applied_moves`

## Global Safety Valve
- The timer starts at the beginning of `optimize`.
- Hard limit: `570.0` seconds.
- `is_time_up()` checks elapsed wall time.
- Phase 1A, Phase 1B, and Phase 2 all stop early if time is up.
- Phase 1A also has an iteration safety limit of `100`.
- Phase 1B also has an attempt safety limit of `20000`.

## Initialization
1. Validate that `tree.root` exists.
2. Build a `DelayModel` from `libs`.
3. Select the smallest buffer for fallback insertion via `choose_smallest_buffer`:
- Primary key: smallest area
- Tie-breaker: smaller nominal delay
4. Initialize `baseline` once:
- `baseline.timing = analyze_timing(...)`
- `baseline.area = compute_tree_area(tree)`
5. Create a `blacklist` for Phase 1B target nodes.

## Phase 1A: Coarse Batched Fixing
Phase 1A is intended to quickly reduce major violations by batching structurally independent targets.
It now includes plateau detection so the batch phase can hand off to Phase 1B once progress becomes too small.

Per iteration:
1. Time check: stop if `is_time_up()`.
2. Oracle call: `current_timing = analyze_timing(...)`.
3. Stop Phase 1A if both SS and FF TNS are already zero.
4. Rank all violating paths by worst slack.
5. Build a batch of targets using independent FF endpoints:
- For each violating path, choose the target node:
  - setup violation: `capture_ff`
  - hold violation: `launch_ff`
- Track required delay per target.
- Only include paths whose launch/capture FFs are not already used in the current batch.
6. For each target in the batch:
- Compute the required delay.
- Use `choose_batch_buffer(required_delay)` to pick the best fitting buffer.
- Insert the buffer above the target using `insert_buffer_between(...)`.
- Record the inserted buffer in the summary.
7. Oracle verify the whole batch.
8. Roll back the batch if either corner TNS degrades:
- If `batch_timing.ss.tns < current_timing.ss.tns` or `batch_timing.ff.tns < current_timing.ff.tns`
- Revert each inserted buffer using the safe structural removal helper `remove_buffer_node(...)`
- Hand off to Phase 1B
9. Compute the total TNS improvement across both corners:
- `tns_improvement = (batch_timing.ss.tns - current_timing.ss.tns) + (batch_timing.ff.tns - current_timing.ff.tns)`
- Baseline improvement threshold: `0.1%` of `abs(baseline.ss.tns) + abs(baseline.ff.tns)`
- If improvement is positive but below threshold, stop Phase 1A without rollback and hand off to Phase 1B

Notes:
- Phase 1A is intentionally coarse and favors structural progress over per-node precision.
- Batch rollback is corner-specific, not based on summed TNS.
- Phase 1A is focused on bulk TNS reduction; Phase 1B is expected to do the finer WNS cleanup.

## Phase 1B: Fine Greedy Cleanup
Phase 1B resolves remaining violations with one-buffer-at-a-time greedy insertion.
It uses an incremental timing cache so each trial only updates the affected clock subtree and the timing path groups connected to affected FFs.

Before the loop:
- Compute SS and FF clock-arrival maps once.
- Build `Phase1bTimingCache` from `ss_paths`, `ff_paths`, and the arrival maps.
- The cache groups timing paths by `(launch_ff, capture_ff)`, indexes groups by FF name, and maintains TNS/WNS/violation counts incrementally.

Per attempt:
1. Time check: stop if `is_time_up()`.
2. Stop if cached SS and FF TNS are both zero.
3. Select the worst remaining violating target using `choose_worst_target(...)` on cached timing.
4. Skip targets already in `blacklist`.
5. Trial insertion:
- Compute `required_delay = -choice.violation_slack`
- Choose an adaptive buffer with `choose_batch_buffer(required_delay)`
- If no buffer fits, fall back to `smallest_buffer`
- Insert that buffer above the target
6. Incrementally verify the trial:
- Update only the inserted buffer subtree in the SS/FF arrival maps.
- Find affected path groups through the FF-to-group index.
- Recompute only those affected path groups.
- Update cached TNS/WNS/violation counts.
7. Keep the insertion only if SS/FF TNS and WNS do not degrade, and at least one of them improves:
- `trial.ss.tns >= previous.ss.tns`
- `trial.ff.tns >= previous.ff.tns`
- `trial.ss.wns >= previous.ss.wns`
- `trial.ff.wns >= previous.ff.wns`
- and at least one of those four metrics is strictly better
8. If the trial fails, roll back the affected path groups and arrival-map updates, undo the inserted buffer with `remove_buffer_node(...)`, and blacklist the target.

Notes:
- Full `analyze_timing(...)` is still used for stage snapshots and final timing; the cache is only a Phase 1B search accelerator.
- Phase 1B uses a small timing epsilon when comparing cached metrics.

## Phase 2: Top-Down Area Recovery
Phase 2 tries to reduce area after timing repair, but it now uses strict corner-preserving acceptance.

Traversal:
- Take a preorder snapshot of the tree with `collect_preorder_names(...)`.
- Process only non-sink, non-original buffer nodes.

For each node:
1. Time check: stop if `is_time_up()`.
2. Attempt removal:
- Temporarily remove the buffer using `remove_buffer_node(...)`
- Oracle call
- Accept only if both TNS and WNS do not degrade in either corner:
  - `trial.ss.tns >= phase2_timing.ss.tns`
  - `trial.ff.tns >= phase2_timing.ff.tns`
  - `trial.ss.wns >= phase2_timing.ss.wns`
  - `trial.ff.wns >= phase2_timing.ff.wns`
- Otherwise restore the node using `restore_buffer_node(...)`
3. If removal is rejected, attempt downsizing:
- Select the next smaller library cell with `choose_next_smaller_buffer(...)` using the actual fanout (`node->children.size()`) for delay estimation
- Temporarily change the buffer type via `set_buffer_type(...)`
- Oracle call
- Accept only with the same strict TNS/WNS non-degradation rule
- Otherwise revert to the original type

## Finalization
After all phases:
1. `final_timing = analyze_timing(...)`
2. `final_area = compute_tree_area(tree)`
3. `final_legality = validate_legality(tree, libs)`
4. `final_score = compute_score_metrics(final_timing, baseline.timing, final_area, baseline.area, alpha, beta, gamma)`
5. `success = final_legality.ok`

If the run completed without timing-limit interruption and legality is OK, the optimizer reports successful completion. If the timer interrupted the run, `early_stopped` is set and the message reflects the early stop.

## Stage Snapshots
The optimizer records a snapshot after each stage:
- `after_phase1a`
- `after_phase1b`
- `after_phase2`
- `phase1a_iteration_snapshots` for every Phase 1A loop iteration

Each snapshot stores:
- stage name
- added / removed / downsized buffer counts
- runtime in seconds
- SS and FF timing metrics
- area
- legality report

These snapshots are used for the report and terminal summaries.

## Key Behavior Summary
- Phase 1A is coarse, batched, and rollback-safe.
- Phase 1B is greedy, adapts buffer size to the required delay, and uses incremental timing updates for trial evaluation.
- Phase 2 is conservative and only accepts changes that preserve both TNS and WNS in both corners.
- The entire optimizer is protected by a hard wall-clock timeout.

## Known Tradeoffs
- Phase 1A can still introduce many buffers when the testcase has widespread violations.
- Phase 1B depends on the buffer library granularity; if no good fit exists, it falls back to the smallest buffer.
- Phase 2 prioritizes safety over aggressiveness, so some area savings may be left on the table.
