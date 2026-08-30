# Large testcase generator

From the project root:

```bash
python3 tools/generate_large_testcase.py
```

The default uses 20 uniquely renamed copies of `testcase0_v2` and writes
`testcase_huge_530k`. It generates 530,240 FFs and approximately 1.59 million
paths per timing corner. A balanced two-level `REALBUF_X16` supertree keeps all
new fanouts at or below five.

The testcase is intended for scalability, deadline, parsing, and memory tests.
Copies do not contain cross-partition datapaths, so it is not a substitute for
an unknown contest testcase when evaluating optimization quality.

Custom example:

```bash
python3 tools/generate_large_testcase.py \
  --source testcase0_v2 \
  --output testcase_huge_custom \
  --copies 20
```
