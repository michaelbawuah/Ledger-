# Matching contract

Ledger is a bounded, single-instrument simulator. Prices are signed 64-bit integer ticks in the configured inclusive interval; quantities are positive 32-bit integers. Order IDs are arbitrary nonzero 64-bit values and must be unique among live orders. IDs may be reused after completion.

Price-time priority chooses the best opposite price, then its oldest resting order. Trades execute at the maker price. Market orders never rest. Limit GTC orders rest their residual; IOC residual expires; FOK checks all eligible liquidity before any mutation and rejects if insufficient. A market command ignores its price and treats GTC/IOC as immediate; market FOK requires full liquidity.

Replace means cancel-replace of the same ID and side, always losing time priority, even for a size reduction. All replacement fields and FOK liquidity are validated before the original order is removed. The original remains intact on rejection. A full book conservatively rejects new GTC limit orders before matching, even if a crossing order could free capacity; IOC/FOK/market need no resting slot. Replacement can reuse its original slot.

Every successful submission emits Accepted. Each execution emits Trade (taker ID, maker contra ID, maker price), maker Filled/PartiallyFilled, then taker Filled/PartiallyFilled. A resting residual has no extra event. Cancel emits Cancelled with its remaining quantity; unfilled immediate residual emits Expired. Replace emits Cancelled then Accepted before fills. Rejected emits one event with its reason. Command sequence numbers are copied into events; callers determine sequence validity.

Caller-owned event storage must have at least `3 * max_orders + 4` slots. An undersized buffer returns OutputFull, clears output, and leaves book/statistics unchanged. This conservative bound prevents partially applied commands and lost execution reports. The buffer is drained after each command; publication/backpressure is the caller's responsibility.

Statistics count accepted new/replacement quantity as submitted; each Trade quantity once as executed; removed live residual as cancelled; immediate residual as expired; rejected input quantity as rejected. Conservation is `submitted = 2 * executed + cancelled + expired + resting_quantity` because both maker and taker quantities were submitted. Rejected quantities are outside conservation.

Public hot-path functions of the fast engine allocate no heap memory, take no locks, and are noexcept. Construction, diagnostics, file I/O, reference engine operations, and workload generation may allocate and throw. Average O(1) ID lookup is not worst-case O(1): bounded open addressing can inspect its table under collisions. Price scans depend on configured range; benchmark both linear and bitmap search.
