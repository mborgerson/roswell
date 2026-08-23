# Code coverage

Line coverage of the kernel as exercised under xemu, without any
guest-side instrumentation: xemu's `-d in_asm` log records the address
and bytes of every translated block, translation happens at first
execution, and the kernel's DWARF line tables map those byte ranges
back to source lines.  `tools/trace2lcov.py` turns the two into an lcov
tracefile that lcov/genhtml and codecov.io consume directly.

Because the measurement is a side effect of running the kernel, any
workload works: the api-regression suite, a title boot, or several runs
combined (trace logs union).

## Recipe

```sh
# 1. A coverage build tree (once).  COVERAGE=ON adds DWARF line info;
#    it forces LTO off because GCC LTO emits no DWARF on PE targets.
cmake -B build-cov -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DENABLE_CCACHE=ON -DCOVERAGE=ON \
      -DCMAKE_TOOLCHAIN_FILE=toolchain-gcc.cmake .

# 2. Run a workload with tracing.  The suite, traced:
NXKRNL_BUILD_DIR=$PWD/build-cov API_REGRESSION_TIMEOUT=300 \
    tools/api-regression-run --trace /tmp/cov-trace.log
# ...or any boot: tools/run-xemu --flash build-cov/flash.bin --trace ...

# 3. Convert to lcov format.
tools/trace2lcov.py -o coverage.info /tmp/cov-trace.log

# 4. Inspect locally (optional; codecov uploads coverage.info as-is).
genhtml -o /tmp/covhtml coverage.info
```

`in_asm,nochain` logging slows execution noticeably -- give bounded
runs a generous timeout.

## CI

The `coverage` variant in `.github/workflows/build.yml` runs this
recipe on every push/PR (the build tree is just `build/`, configured
with `-DCOVERAGE=ON`) and uploads `coverage.info` to codecov.io, plus
as a build artifact.  Repo-side codecov behavior lives in
`codecov.yml`; statuses are informational, not PR-gating.  Uploads use
`CODECOV_TOKEN` if the repository secret is set; without it, public
repos fall back to tokenless upload (rate-limited).

## What the numbers mean

* A line counts as hit if any instruction attributed to it was
  *translated*, which for TCG equals "executed at least once".  There
  are no execution counts -- every DA record is 0 or 1.
* Inlined code is attributed to the callee's source line, so headers
  and inlined helpers get real coverage.
* The denominator is the set of lines with code in the coverage image.
  The non-LTO layout keeps some branches that the shipped LTO link
  folds away (see below), so a few permanently-red regions are
  expected -- e.g. the retained ARM3 paths.
* Assembly sources (`.s`/`.S`) carry no directory in their DWARF and
  are dropped from the report.

## How the coverage build differs from the shipped image

`-DCOVERAGE=ON` only adds `-g` flags -- the flash payload is stripped
either way.  But it forces `LTCG=FALSE`, and the non-LTO image differs
from the shipped one in two audited ways:

* The nxmm configuration removes ARM3 source files whose remaining
  callers sit in branches that cannot execute on Xbox.  LTO folds those
  branches away; a plain link needs the symbols, which
  `ntoskrnl/xb/mm/arm3stubs.c` provides as bugchecking stubs.
* Boot-only callers stay resident instead of being migrated into INIT,
  so the post-build resident->INIT reference audit (`init-audit.py
  --check`) would report false positives; COVERAGE builds skip it.

Both also apply to any `-DLTCG=FALSE` iteration build; the stubs are
what keep that combination linking.
