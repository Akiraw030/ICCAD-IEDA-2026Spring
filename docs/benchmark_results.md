# Final Local Benchmark Results

This page preserves the final aggregate results after removing the large raw
timing reports from the public repository.  It is a local reproducibility
record, not an official contest result.

## Method

- Build: Release (`-O3 -DNDEBUG`), with optimizer timing-report output disabled.
- Runtime limit: 570 seconds configured for the optimizer; the measured wall
  time below also includes process startup and output overhead.
- Validation: the official checker was run after optimization.
- Inputs and checker: obtain them from the official Problem D download page;
  they are intentionally not redistributed by this repository.

## Final Results

| Case | Release wall time (s) | SS WNS | SS TNS | SS NVP | FF WNS | FF TNS | FF NVP | Buffer area |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| testcase0 | 526.33 | 0 | 0.0000 | 0 | 0 | 0.0000 | 0 | 100.6590 |
| testcase1 | 527.80 | 0 | 0.0000 | 0 | 0 | 0.0000 | 0 | 160.9818 |
| testcase2 | 527.96 | 0 | 0.0000 | 0 | 0 | 0.0000 | 0 | 404.4495 |
| testcase3 | 531.59 | -0.0013 | -0.0042 | 6 | 0 | 0.0000 | 0 | 1279.9225 |
| testcase4 | 515.24 | -0.0038 | -0.0268 | 24 | 0 | 0.0000 | 0 | 5169.3884 |
| testcase0_v2 | 514.60 | -0.0399 | -7.9176 | 1807 | 0 | 0.0000 | 0 | 2846.2734 |
| testcase1_v2 | 531.04 | -0.0001 | -0.0004 | 4 | 0 | 0.0000 | 0 | 734.0499 |

`testcase530` is an internally generated stress case derived for scalability
testing.  It is not an official benchmark and is therefore intentionally
excluded from this public comparison.

## Interpretation

The final configuration closes both timing corners on the three smaller
official cases.  On the larger official cases, FF timing is closed and only
small SS residual violations remain, except for `testcase0_v2`, which remains
the most difficult public case in this final local run.

Detailed design evolution and A/B conclusions are documented in
[`strategy0.md`](strategy0.md) through [`strategy8.md`](strategy8.md).
