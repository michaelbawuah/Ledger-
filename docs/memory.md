# Pool and order-ID index contracts

`include/ledger/pool.hpp` and `include/ledger/id_index.hpp` implement the fixed
storage used by the matching engine. They are internal components; the book
validates commands before calling them. Their storage is allocated during
construction. Acquiring, releasing, finding, inserting, and erasing do not grow
the backing vectors.

## Order pool

The pool holds a fixed number of nodes. A slot is either live or reachable from
the free list. The initial list yields slots in ascending order; a released slot
joins the head and is acquired before slots released earlier. Acquire clears all
order fields and intrusive queue links, then marks the node live. Release clears
the fields, marks the node free, and links it into the free list.

- `acquire()` returns `no_slot` at capacity without changing the pool.
- A zero-capacity pool is valid, empty, and full.
- `release(slot)` requires an in-range live slot owned by this pool. Double
  release and release of an arbitrary integer are caller errors, not checked
  rejection paths.
- Node references remain stable for the lifetime of the pool, but their payload
  belongs to the current acquisition of the slot.
- `check_invariants()` checks the live/free partition, cardinality, and free-list
  bounds and cycles. It allocates diagnostic scratch space and belongs outside
  latency measurements.

The standalone `tests/unit/pool_tests.cpp` suite tests zero/full capacity, stable
exhaustion, payload reset, LIFO reuse, every release permutation at capacities
1–6, and 400,000 seeded operations against an independent slot/free-stack oracle.
It also deliberately corrupts fresh diagnostic-only pools to prove that cycles,
invalid edges, overlapping ownership, and unowned slots are detected. Those
negative fixtures never call acquire or release after corruption.

Build and run from the repository root:

```sh
c++ -std=c++23 -O2 -Wall -Wextra -Wpedantic -Iinclude tests/unit/pool_tests.cpp -o pool_tests
./pool_tests
```
