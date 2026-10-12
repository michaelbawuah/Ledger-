# Resume wording grounded in the recorded evidence

**Ledger - Limit Order Book & Matching Engine | C++23, CMake, GoogleTest, libFuzzer**

- Built an allocation-free C++23 price-time matching engine with intrusive FIFO queues, a fixed order arena and bitmap price search; measured 19.24M commands/s and 271 ns p99 on a 1M-command synthetic replay, 2.19x an independent STL reference on the same AMD EPYC host.
- Verified matching against the reference across 100M randomized commands with zero mismatches; added binary-feed fuzzing, ASan/UBSan checks and a TSan-verified SPSC feed adapter.

Short single-bullet version:

- Engineered Ledger, an allocation-free C++23 limit order book with price-time priority and lock-free SPSC handoff; processed 19.24M synthetic commands/s (271 ns p99) and verified 100M randomized commands against an independent reference engine.

Interview limits: these are synthetic matching-core results on the documented
virtual host. The project includes a synthetic market maker but has no live
exchange connection. Remote CI is configured, not yet observed passing. Full
structural checks run at checkpoints, while report equality and observable
invariants run after every differential command.
