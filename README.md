# CADD0045 — Clock-Tree Delay Optimizer

A C++17 optimizer for the 2026 ICCAD Contest Problem D, *Timing Fixing by
Useful Skew*. It improves clock-tree timing and buffer area through legal buffer
resize, insertion, and removal operations.

## Competition Assets

Testcases, the checker, specification, and Q&A are provided by the contest and
are not redistributed here. Download them from the official
[Problem D page](https://www.iccad-contest.org/tw/03_problems.html).

Each testcase directory must contain:

```text
clk_tree.structure
buf.lib
SS_delay.rpt
FF_delay.rpt
```

## Build

Requirements: CMake and a C++17 compiler. Release builds use `-O3 -DNDEBUG`.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DENABLE_OUTPUT=OFF
cmake --build build --parallel
```

The executable is `build/cadd0045`. Use `-DENABLE_OUTPUT=ON` to generate a
timing report beside each output tree.

## Run One Testcase

```bash
./build/cadd0045 \
  /path/to/testcase_root/testcase0 \
  /path/to/output/modified_clk_tree.structure
```

The output directory must already exist.

## Run Public Testcases

`run_reports.sh` builds with reports enabled and runs the seven official public
workloads:

```bash
./run_reports.sh /path/to/testcase_root [output_directory]
```

`run_final.sh` performs a report-enabled pass and a quiet Release pass, then
runs the official checker:

```bash
./run_final.sh /path/to/testcase_root /path/to/checker/checker [output_directory]
```

Set `CADD0045_TESTCASES` to a whitespace-separated testcase subset when needed.
Generated output is written below `report/` and is ignored by Git.

## Documentation

- [Architecture and pipeline](docs/architecture.md) — current stages,
  objectives, acceptance rules, and runtime policy.
- [Timing engine](docs/timing_engine.md) — transactional timing trials and
  rollback design.
- [Search experiments](docs/search_experiments.md) — retained and rejected
  search strategies.
- [AI-assisted optimization note](docs/ai_optimization_note.md) — concise note
  on the iterative optimization workflow.
- [Benchmark results](docs/benchmark_results.md) — aggregate local Release and
  checker results.
- [Design history](docs/design_history.md) — compact Strategy0–Strategy8
  evolution.

## Source Layout

- `src/optimizer.*`: optimization policy and configuration.
- `src/fast_timing_engine.*`: transactional local timing backend.
- `src/delay_model.*`: full timing and legality analysis.
- `src/parser.*`, `src/io.*`, `src/tree.*`: input, output, and clock-tree data
  structures.

`OptimizerConfig` in [`src/optimizer.h`](src/optimizer.h) is the authoritative
place for tunable parameters. The current pipeline documentation is a guide;
the source code is authoritative.
