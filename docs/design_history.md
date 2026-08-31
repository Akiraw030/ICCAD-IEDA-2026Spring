# Design History

This page is a compact record of the optimizer's evolution.  It replaces the
superseded, implementation-level Strategy0–Strategy6 snapshots, which repeated
large portions of the timing model and helper documentation.  For the current
behavior, use [`architecture.md`](architecture.md); the source code is authoritative.

| Strategy | Main change | Lasting outcome |
|---|---|---|
| 0 | Oracle-driven greedy buffer insertion followed by conservative area recovery. | Established the baseline timing-repair / area-reclaim separation. |
| 2 | Coarse batched repair, fine-grained repair, and strict corner-preserving reclaim. | Established batch rollback and the coarse-to-fine repair idea. |
| 3 | Incremental path-group timing cache. | Made local timing trials practical, but was later superseded by transactional timing updates. |
| 4 | Phase0 multi-pass resizing of existing buffers before insertion. | Retained as the first optimization stage. |
| 5 | Repair/Reclaim cycles and Final Alternating Greedy. | Established the recurring repair–area tradeoff used by later pipelines. |
| 6 | FastTimingEngine integration and runtime cleanup. | Introduced the fast transactional trial backend. |
| 7 | Transactional timing-engine completion, rollback refinement, and expanded FinalAlt search. | See [`timing_engine.md`](timing_engine.md). |
| 8 | Perturb-and-recover experiments, best-state checkpoints, and deadline-aware multi-start search. | See [`architecture.md`](architecture.md) and [`search_experiments.md`](search_experiments.md). |

## Documentation Guide

- [`architecture.md`](architecture.md): current stages, parameters, acceptance rules,
  stop conditions, and known limitations.
- [`timing_engine.md`](timing_engine.md): timing-engine architecture and
  validation strategy.
- [`search_experiments.md`](search_experiments.md): retained and rejected search
  ideas.
- [`benchmark_results.md`](benchmark_results.md): retained aggregate local
  Release/checker benchmark.

Raw testcase files, checker binaries, and raw experiment reports are not part
of this repository.  Obtain official assets from the contest website cited in
the project README.
