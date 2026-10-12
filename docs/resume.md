# Resume wording grounded in the recorded evidence

**Ledger - Limit Order Book & Matching Engine | C++23, CMake, GoogleTest, libFuzzer**

- Built an allocation-free C++23 price-time matching engine with intrusive FIFO queues, a fixed order arena and bitmap price search; measured 18.74M commands/s and 271 ns p99 on a 1M-command synthetic replay, 2.27x an independent STL reference on the same AMD EPYC host.
- Verified matching against the reference across 100M randomized commands with zero mismatches; added binary-feed fuzzing, ASan/UBSan checks and a TSan-verified SPSC feed adapter.

Short single-bullet version:

- Engineered Ledger, an allocation-free C++23 limit order book with price-time priority and lock-free SPSC handoff; processed 18.74M synthetic commands/s (271 ns p99), achieved 2.27x reference throughput, and verified 100M randomized commands against an independent reference engine.

These numbers use the current [core-v2 measurement packet](../bench/results/core-v2/summary.json),
also used by the README and [measurement report](results.md). The throughput is
the median of five runs; 271 ns is the median run p99, including clock overhead.

Interview limits: these are synthetic matching-core results on the documented
virtual host. The project includes a synthetic market maker but has no live
exchange connection. All nine runnable checks passed in the
[verified GitHub Actions run](https://github.com/michaelbawuah/Ledger/actions/runs/38194384600),
including GCC/Clang, macOS/Windows, sanitizers, parser fuzzing, and study-figure
reproduction. The [main-branch workflow](https://github.com/michaelbawuah/Ledger/actions/workflows/ci.yml?query=branch%3Amain)
shows the current status. Full structural checks run at checkpoints, while
report equality and observable invariants run after every differential command.
