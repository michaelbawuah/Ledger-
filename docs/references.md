# Design references

Ledger is an original C++23 implementation, not a fork of either project below. These repositories informed the design discussion; their implementation code is not included. Ledger's matching rules and limits are defined in [semantics.md](semantics.md), and its own tests and measurements establish its behavior.

| Reference | Design lessons considered |
| --- | --- |
| [brprojects/Limit-Order-Book](https://github.com/brprojects/Limit-Order-Book) | Separate price levels from orders, maintain FIFO priority within each level, index live orders by ID, and explain latency through the work performed by each command. |
| [jxm35/LimitOrderBook-MatchingEngine](https://github.com/jxm35/LimitOrderBook-MatchingEngine) | Separate the matching core from order entry and market-data publication, use integer prices, and isolate core benchmarks from publisher overhead. |

Ledger makes different implementation choices: bounded price arrays, a preallocated order arena, index-linked FIFO queues, bounded ID lookup, caller-owned event storage, and an independently implemented ordered-map reference engine. The bounded configuration is an explicit tradeoff rather than a claim of universal superiority.

## Reading benchmark results

The references' published figures describe their workloads and environments. The brprojects README reports about 1.4 million requests per second from a synthetic workload on an Intel i5-12450H. The jxm35 README reports about 14 million orders per second in simulation; its [benchmark implementation](https://github.com/jxm35/LimitOrderBook-MatchingEngine/blob/main/benchmarks/OrderBookBenchmarks.cpp) uses a null market-data publisher. These figures are context, not Ledger results or directly comparable baselines.

Ledger reports measurements from its own executable, with workload, build and machine details. Matching-core latency excludes network transit, client processing and external venue execution. TCP loopback measurements, where provided, are reported separately. Synthetic order flow demonstrates simulation and systems behavior; it does not establish live trading performance.

## Attribution and licensing

The brprojects repository includes an [MIT license](https://github.com/brprojects/Limit-Order-Book/blob/main/LICENSE), copyright Benjamin Rienecker (2024). No license file was present in the jxm35 repository tree reviewed on 2026-10-12, and its GitHub metadata did not declare a license. Both remain linked design references; Ledger contains original code under its own license.
