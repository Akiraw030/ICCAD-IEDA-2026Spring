# Optimizer Architecture

This page describes the current production pipeline. The detailed defaults are
in [`../src/optimizer.h`](../src/optimizer.h), which is authoritative.

## Objective

The optimizer uses normalized SS timing, FF timing, and clock-tree area terms.
The normal search weights are `0.20 / 0.20 / 0.60`. When a deadline-aware
portfolio compares complete routes, it selects the best legal endpoint using a
separate timing-heavy target weight of `0.64 / 0.18 / 0.18`.

Score-based acceptance never bypasses legality, buffer-library, fanout, or
deadline checks.

## Pipeline

```text
Parse input and build the clock tree
        ↓
Full baseline timing + area analysis
        ↓
Phase0: pressure-guided resize of existing buffers
        ↓
Existing-buffer shared repair/reclaim cycle
        ↓
Mandatory full-score repair route (safe fallback)
        ↓
Optional deadline-aware portfolio routes
        ↓
Final Alternating Greedy repair/reclaim
        ↓
Restore best legal route → full timing, area, and legality validation
```

### Phase0

Phase0 ranks existing buffers by timing pressure and estimated useful delay
change, then evaluates legal resize candidates with the transactional timing
engine. The default mode evaluates nodes individually; batch mode remains an
optional experiment. A resize is kept only when the enabled acceptance policy
improves the state.

### Repair/Reclaim

Repair Pulse inserts buffers for timing violations. Reclaim then downsizes or
deletes legal candidates to recover area. Rejected repair batches may split and
retry; repeated failures reduce pulse scale and temporarily avoid failed
targets. A score-based policy is enabled by default, while legacy TNS guards
remain available for A/B testing.

The optional existing-buffer shared stage inserts a buffer between an existing
parent and compatible children, preserving all original ancestor/descendant
relations. It is skipped on cases where prior measurements showed no return.

### Final Alternating Greedy

FinalAlt alternates endpoint repair with ranked reclaim candidates. It retains
the legacy heuristic acceptance policy by default and has a target-score
checkpoint for its best observed endpoint.

### Deadline-Aware Portfolio

One complete fallback route always runs first. If safe time remains, the
optimizer explores independent routes with different acceptance policies,
objective schedules, and initial buffer-size perturbations. It never begins a
route without reserving enough time for final validation and output.

The older perturb-and-recover stage is implemented but disabled by default: its
cost was not justified by retained QoR gains on large cases.

## Timing and Legality

`FastTimingEngine` handles local resize, insert, delete, commit, and rollback
transactions. Full analysis is still used at stage boundaries and before output.
The final output is accepted only after full timing, area, and legality
validation.

## Runtime Policy

The optimizer has a 570-second global limit. Phase0, Repair/Reclaim, and
portfolio routes also make deadline checks inside long loops. Case-size-aware
finalization reserves prevent the optimizer from overrunning the contest wall
clock limit during validation or serialization.

## Configuration

All tunable values are grouped and commented in
[`OptimizerConfig`](../src/optimizer.h). Important groups are global objective
and score acceptance, Phase0, Repair/Reclaim, FinalAlt, portfolio/deadline
reserves, and optional verification checks.
