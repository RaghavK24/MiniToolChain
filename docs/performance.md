# Performance

Every figure below comes with the machine and build that produced it.
The benchmark prints that information at the top of its output, so a
copied number keeps its context.

```
build/msvc-release/bench_toolchain.exe [iterations]
```

## Measurement environment

```
compiler   : MSVC 19.44 (Visual Studio 2022 Build Tools 17.14)
build      : release, /O2 /DNDEBUG
platform   : windows-x64
cpu        : Intel64 Family 6 Model 140 Stepping 1 (11th-gen mobile i7 class)
iterations : 20
```

## Results

Times are per iteration, in microseconds.

| Benchmark | mean | p50 | p95 | p99 | throughput |
|---|---|---|---|---|---|
| lexer | 300 | 301 | 322 | 322 | 242 MB/s |
| parser | 2686 | 2516 | 3548 | 3548 | 29 MB/s |
| assemble (-O0) | 4808 | 4757 | 7363 | 7363 | 841 K instructions/s |
| assemble (-O1) | 5301 | 4899 | 8076 | 8076 | 817 K instructions/s |
| object write | 914 | 898 | 1023 | 1023 | 201 MB/s |
| object read | 843 | 843 | 905 | 905 | 214 MB/s |
| executable read | 699 | 655 | 922 | 922 | 220 MB/s |
| link | 437 | 438 | 494 | 494 | 9.1 M instructions/s |
| vm execution | 4276 | 4221 | 5191 | 5191 | **23.7 M instructions/s** |
| vm with tracing | 5651 | 5805 | 6301 | 6301 | 17.2 M instructions/s |
| disassemble | 3842 | 3703 | 5568 | 5568 | 1.1 M instructions/s |

The input is a generated ~4000-instruction program with loops, calls and
data; the VM figure comes from a 100 000-instruction loop.

## What the numbers say

**The VM runs about 24 million instructions per second** with a plain
`switch` dispatch loop and a bounds- and permission-checked memory access
on every load and store.
That is the number to beat if dispatch is ever worth optimizing. See
below for why it has not been.

**Tracing costs about 27%.** It is a `std::function` call per instruction, and the trace is off by default. That is a
reasonable price for the debugger using the same VM the user runs.

**The parser is 8x slower than the lexer** and dominates assembly time.
It allocates: a `std::string` per mnemonic, a `std::vector<Operand>` per
instruction, an `ast::Operand` carrying two strings. If assembly time
ever mattered, that is where to start, not the lexer, which is
already at memory-copy speed.

**Optimizing costs about 10% of assembly time** (4757 -> 4899 µs) and
removes 12.5% of executed instructions on this program. On
`examples/optimization.asm`, written to be reducible, it removes 58%.

**The binary formats read and write at 200+ MB/s** despite validating
every offset, length and index and computing a CRC-32 over the file. The
"validation is expensive" instinct is wrong at this scale.

## What has not been optimized, and why

Nothing. The project was built for correctness first, and the profile
above is the first measurement: measure first, then decide what to
optimize.

Two candidates stand out:

* **`findOpcode` is a linear scan** of 36 entries, called on nearly every
  instruction in every stage. A 256-entry lookup table would be trivially
  faster. It has not been done because nothing in the profile says it is
  the bottleneck, and the linear scan is obviously correct.
* **The VM's dispatch is a `switch`.** A function-table dispatch is the
  usual alternative. The comparison has not been run,
  so no claim is made about which is faster here. That experiment needs
  its own measurement. Until then, all this says is that the current loop
  does 24M instructions/s.

## Methodology

* Every benchmark runs once untimed before the measured iterations, so
  cold caches and lazy allocation do not land in the first sample.
* Timing uses `std::chrono::steady_clock` around one iteration, and the
  report gives mean, p50, p95 and p99, since a single mean would hide the
  scheduler noise visible in the p95 column here.
* Setup (reading files, building inputs) happens outside the timed
  region.
* Throughput is computed from p50, not from the mean, so an outlier does
  not flatter it.

## Reproducing

```bash
pwsh tools/build.ps1 -NoTests
build/msvc-release/bench_toolchain.exe 20
```

or with CMake:

```bash
cmake --preset release && cmake --build build/release
./build/release/benchmarks/bench_toolchain 20
```

Quote a number from a *release* build only. The benchmark prints a
warning in its header when it was built without `NDEBUG`, because a debug
figure is off by an order of magnitude and is not comparable to anything.
