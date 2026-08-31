# CADD0045 — Clock Tree Delay Optimizer

This repository contains the C++ optimizer for the clock-tree delay optimization project. The submitted executable is named `cadd0045`.

## Competition Assets

The contest testcase, checker, specification, and Q&A are not redistributed by
this repository. Download them from the official
[2026 ICCAD Contest Problem D page](https://www.iccad-contest.org/tw/03_problems.html),
then place the public testcase directories under one directory of your choice.
The scripts below call this directory `<testcase_root>`.

## Overview

The program reads one testcase directory, optimizes the clock tree, and writes the modified clock tree to the output path requested on the command line:

```bash
./cadd0045 <testcase_dir_absolute_path> <output_file_absolute_path>
```

The testcase directory must contain:

- `clk_tree.structure`
- `buf.lib`
- `SS_delay.rpt`
- `FF_delay.rpt`

The output file's parent directory must already exist. The program writes exactly to the second command-line argument.

By default, the executable prints terminal summaries and writes timing report
files. Set the CMake option `ENABLE_OUTPUT=OFF` when a quiet submission binary
is needed. Optimizer behavior and verification modes are controlled from
`OptimizerConfig` in `src/optimizer.h`, not from CMake options.

## Build

For single-config generators, the project defaults to `Release`
(`-O3 -DNDEBUG`) when `CMAKE_BUILD_TYPE` is not specified.

Recommended optimized build with timing reports:

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DENABLE_OUTPUT=ON
cmake --build build
```

The executable is generated at:

```text
build/cadd0045
```

If you copy the project to another machine or server, do not reuse an old copied `build/` directory. Remove it or create a fresh build directory, otherwise CMake may complain that `CMakeCache.txt` was created from a different source path.

To produce a quiet optimized submission binary, reconfigure the same build
directory instead of creating another one:

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DENABLE_OUTPUT=OFF
cmake --build build
```

With `ENABLE_OUTPUT=ON`, the program writes `timing_report_<testcase>.txt` beside the output tree and prints optimization summaries. With `ENABLE_OUTPUT=OFF`, it still writes the same `modified_clk_tree.structure`; it just skips reports and terminal summaries.

For development and source-level debugging, override the default explicitly:

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Debug \
  -DENABLE_OUTPUT=ON
cmake --build build
```

For optimized builds that retain debug symbols, use
`-DCMAKE_BUILD_TYPE=RelWithDebInfo` (normally `-O2 -g -DNDEBUG`) with the same
`build/` directory. Reconfiguring changes the mode of that one build; running
`run_reports.sh` changes it back to Release with output enabled.

## Run

Example from the project root, after downloading the official assets:

```bash
./build/cadd0045 \
  /path/to/testcase_root/testcase0 \
  /path/to/output/modified_clk_tree.structure
```

Example from inside `build/`:

```bash
cd build
./cadd0045 "$(realpath ../testcase0)" "$(realpath ../testcase0)/modified_clk_tree.structure"
```

Relative paths also work if they are valid from the current working directory, but absolute paths are safest and match the contest command format.

## Batch Reports

`run_reports.sh` always reconfigures the shared `build/` directory with
`CMAKE_BUILD_TYPE=Release` (`-O3`) and `ENABLE_OUTPUT=ON`, then runs:

- `testcase0`
- `testcase1`
- `testcase2`
- `testcase3`
- `testcase4`
- `testcase0_v2`
- `testcase1_v2`

Usage:

```bash
./run_reports.sh /path/to/testcase_root
```

Reports are saved under:

```text
report/run_<timestamp>/
```

You can also choose a destination:

```bash
./run_reports.sh /path/to/testcase_root report/my_experiment
```

## Final Full-Suite Record

`run_final.sh` produces the final two-pass record under `report/final/`. It
requires a testcase root and the path to the official checker executable:

```bash
./run_final.sh /path/to/testcase_root /path/to/checker/checker
```

The first pass builds Release/O3 with `ENABLE_OUTPUT=ON` and retains each
timing report, output tree, process logs, and runtime. The second pass rebuilds
the same optimizer with `ENABLE_OUTPUT=OFF`, retains the submission-form output
tree, and runs the official checker. Exact CMake caches, compiler flags, binary
hashes, and a combined `summary.tsv` are also saved.

The default testcase list is the same seven official public workloads used by
`run_reports.sh`. To run a custom list:

```bash
CADD0045_TESTCASES="testcase0 testcase2" \
  ./run_final.sh /path/to/testcase_root /path/to/checker/checker
```

The script refuses to overwrite a non-empty `report/final/`; move or remove an
older final record explicitly before starting another complete run.

## Current Optimizer Flow

The current implementation is documented in [docs/strategy8.md](docs/strategy8.md).
Strategy 7 remains available as the timing-engine and inherited-pipeline history.

Default flow:

```text
Phase0 -> mandatory full_score fallback
       -> deadline-aware multi-start portfolio
       -> restore best legal target-score checkpoint -> full validation
```

Important defaults in `OptimizerConfig`:

- Global time budget: `570` seconds
- Weighted search objective: `alpha=0.20`, `beta=0.20`, `gamma=0.60`
- Portfolio winner objective: `0.64/0.18/0.18`
- Independent score-acceptance switches for Phase 0, Repair/Reclaim, and
  FinalAlt; Phase 0 and Repair/Reclaim are enabled, while FinalAlt is disabled
  by default
- One best-state checkpoint is available but disabled by default
- Phase0 enabled, non-reset mode by default
- Phase0 uses individual FastTimingEngine trials by default; automatic batching
  is disabled
- Phase0 uses pressure-weighted average SS/FF delay change minus area-growth
  penalty for ranking; score-aligned estimated ranking is disabled
- Optional Phase0 batches use one native multi-resize timing transaction,
  indexed ancestor checks, and a bounded split depth
- Phase0 total candidate budget: unlimited (`0`)
- Phase0 safety time budget: `180` seconds
- Phase0 tries up to `6` types per node for at most `5` passes
- Repair/Reclaim cycles enabled until the absolute `360` second boundary
- Each reclaim scan has a `120` second local safety budget
- Rejected Repair Pulse batches split recursively to maximum depth `2`
- Exhausted Repair Pulses use the enabled scale-reduction/temporary-blacklist
  recovery policy
- Repair Pulse uses score-not-worse acceptance by default. Its guarded-OR TNS
  rule remains the fallback when Repair/Reclaim score acceptance is disabled
- Adaptive Repair/Reclaim parameters enabled
- Final Alternating Greedy enabled as the legacy final-stage replacement; its
  local budget is disabled, but it reserves Strategy8 and validation time
- FinalAlt tries up to `500` repair insertions per iteration, retries failed targets after a `3`-iteration cooldown, and
  permanently blacklists them after `3` failures
- FinalAlt tries up to `3` repair buffer types per target and `3` smaller
  reclaim types per node
- FinalAlt score acceptance remains available but is disabled by default
- Strategy8 brutal maximum-area-path perturb-and-recover remains available but
  is disabled by default because its measured recovery routes added runtime
  without improving the retained checkpoint
- After one mandatory complete fallback, the portfolio spends safe remaining
  time on 5%/10%/20% pre/post-Phase0 buffer-resize initial states with rotating
  deterministic seeds and score/heuristic/cosine policy variants
- Route count is deadline-controlled. The 570-second optimizer limit and
  small/medium/large finalization reserves of 10/30/60 seconds keep the run
  below the contest's 600-second wall-clock limit
- Each perturbation selects the not-yet-tried leaf-to-root path with the largest
  cumulative buffer area among paths containing a deletable buffer. Every
  deletable buffer on that path is removed; original and otherwise
  non-deletable buffers remain completely unchanged
- Brutal mode skips the small Quick FinalAlt trials. The raw perturbed tree goes
  directly into a reclaim-first Repair/Reclaim variant: unlimited bootstrap
  reclaim, unlimited Repair Pulse targets, and unlimited normal reclaim trials,
  bounded by the stage and per-reclaim time limits
- Full recovery may run for up to `200` cycles. A per-recovery-cycle checkpoint
  prevents a late recovery cycle from overwriting an earlier higher-score
  endpoint
- Strategy8 score-only acceptance requires legality and a strict weighted-score
  improvement; individual TNS/WNS/NVP/Area metrics may regress. Optional timing
  and NVP guards are disabled. Rejected cycles restore the current checkpoint,
  and the stage always emits its best checkpoint
- Set `perturb_recover_use_brutal_area_path=false` to restore the v6 profiled
  Timing/Area/Balanced/Random beam perturbation and bounded reclaim-first
  recovery
- FastTimingEngine enabled
- Fast timing verification disabled by default

The current pipeline, benchmark, and checkpoint behavior are documented in
[docs/pipeline.md](docs/pipeline.md).

To recover the legacy final Phase1B plus Phase2 cleanup, set either:

```cpp
enable_final_alternating_greedy = false;
```

or:

```cpp
final_alt_replace_phase1b_and_phase2 = false;
```

To run the optional reset-based Phase0 comparison experiment, set:

```cpp
enable_phase0_reset_experiment = true;
```

## Verification

Expensive consistency checks are off by default for normal runtime. They can be enabled in `src/optimizer.h`, including:

- `enable_timing_cache_verify`
- `enable_phase0_incremental_verify`
- `enable_fast_timing_verify`
- `enable_phase0_trial_full_validation_verify`
- `enable_final_alt_trial_full_validation_verify`

The official checker usage is:

```bash
<checker_binary> \
  <clk_tree.structure absolute path> \
  <modified_clk_tree.structure absolute path> \
  <buf.lib absolute path> \
  <FF_delay.rpt absolute path> \
  <SS_delay.rpt absolute path>
```

During local testing, generated `modified_clk_tree.structure` files passed the
checker after using a temporary checker-friendly copy of `buf.lib`. The bundled
checker requires whitespace between `cell` and `(`; for example it rejects
`cell(REALBUF_X16)` but accepts `cell (REALBUF_X16)`. The optimizer parser
handles both forms.

## Files of Interest

- `src/main.cpp`: command-line entry point.
- `src/parser.cpp`: parses `clk_tree.structure`, `buf.lib`, and timing reports.
- `src/io.cpp`: writes `modified_clk_tree.structure`; indentation uses tabs.
- `src/tree.h` / `src/tree.cpp`: in-memory clock-tree representation and mutation APIs.
- `src/delay_model.h` / `src/delay_model.cpp`: timing and legality model.
- `src/optimizer.h` / `src/optimizer.cpp`: optimizer configuration and implementation.
- `src/fast_timing_engine.h` / `src/fast_timing_engine.cpp`: transactional fast timing backend for local trials.
- `docs/strategy8.md`: current deterministic perturb-and-recover implementation and A/B results.
- `docs/strategy7.md`: timing-engine changes and Strategy7 benchmark history.
- `docs/pipeline.md`: current Strategy8 pipeline and configuration.
- `docs/strategy6.md`: inherited optimization-policy documentation.
- `run_reports.sh`: local report-generation helper; takes an external testcase root.
- `run_final.sh`: two-pass optimizer/checker runner; takes external testcase and checker paths.
- `docs/benchmark_results.md`: retained aggregate local benchmark results.

Historical strategy documents are kept in `docs/strategy2.md` through
`docs/strategy7.md`.
