# Search Experiments

The final optimizer was developed through controlled local experiments. Raw
reports are intentionally excluded from this public repository; retained
aggregate results are available in [`benchmark_results.md`](benchmark_results.md).

## Retained Ideas

- **Pressure-guided Phase0 resizing** conditions the original tree before
  insertion-based repair.
- **Transactional local trials** make a wider search practical within the
  runtime budget.
- **Repair/Reclaim alternation** trades timing gain for area recovery while
  retaining rollback rules.
- **Deadline-aware multi-start portfolio** always completes one fallback route,
  then spends safe remaining time exploring independent initial states and
  policies.
- **Target-score checkpoint selection** chooses the best legal completed route
  using a timing-heavy selector distinct from the area-oriented search score.

## Retained but Disabled Options

- score-weight cosine scheduling;
- shared-path repair on existing buffer edges;
- perturb-and-recover after FinalAlt; and
- alternative repair-pulse scales and candidate-ranking modes.

They remain useful for controlled A/B experiments but are not production
defaults because their average QoR gain did not justify their runtime cost.

## Rejected Directions

- aggressive post-FinalAlt perturbation on large cases;
- smaller FinalAlt reclaim caps that reduced candidate diversity;
- compiler `native`/LTO variants without repeatable speedup; and
- unrestricted score-only moves that traded substantial timing quality for
  small area gains.

## Reproducibility

The portfolio seeds are deterministic, and each completed route is fully
validated before comparison. Experiment controls remain grouped in
`OptimizerConfig`; see [`architecture.md`](architecture.md) for the current
flow and [`design_history.md`](design_history.md) for the Strategy timeline.
