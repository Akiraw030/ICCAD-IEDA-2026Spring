# Transactional Timing Engine

`FastTimingEngine` is the optimizer's local-trial backend. It avoids a full
timing recomputation for every candidate while keeping trial mutations
reversible.

## Supported Local Operations

- resize an existing buffer;
- insert a buffer on a legal edge;
- delete a legal optimizer-inserted buffer; and
- commit or roll back a single move or a batch transaction.

The engine maintains clock arrivals, affected timing aggregates, path-related
state, and area deltas for the impacted region. A rejected candidate restores
the exact pre-trial state instead of rebuilding the complete tree.

## Where It Is Used

- Phase0 resize trials;
- Repair/Reclaim insert, resize, and delete trials;
- FinalAlt reclaim trials; and
- optional recovery routes.

Full `DelayModel` analysis is intentionally retained at pipeline boundaries,
for final validation, and whenever detailed path information is needed for
candidate selection.

## Correctness Strategy

Every transaction checks structural legality before it is accepted. Optional
verification switches in `OptimizerConfig` compare fast timing and incremental
area values against the full model. The final emitted tree always receives a
full timing, area, and legality analysis, so an incremental cache is never the
sole final authority.

## Design Tradeoff

The engine is optimized for many small local trials. It does not attempt to
replace all full analyses: doing so would lose the path-level information used
to rank repair and reclaim candidates. This hybrid design provided most of the
runtime benefit while preserving transparent validation points.
