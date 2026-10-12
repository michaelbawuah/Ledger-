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

## Order-ID index

The ID index uses an open-addressed table rounded up to a power of two, with at
least twice the configured maximum live order count. A zero key marks an empty
entry. Nonzero IDs use all 64 bits, including high-bit-only values and
`UINT64_MAX`. Lookup and erase stop at the first empty entry; lookup of zero
returns `no_slot` and erase of zero returns false.

`insert(id, slot)` requires a nonzero absent ID, a nonsentinel slot, and available
capacity. It is an unchecked internal insertion primitive: duplicate IDs,
sentinel IDs, and overfilling are caller errors. `FastBook` enforces these
preconditions. A failed erase of an absent ID does not alter the table.

Erasure closes a probe-cluster hole by moving only displaced entries whose probe
path crosses that hole. Entries already at their home bucket are skipped; later
entries can still move past them. The distance calculation wraps around the
physical end of the table. The final hole is reset to an empty entry, avoiding
tombstones that could accumulate during repeated cancellations.

The standalone `tests/unit/index_tests.cpp` suite covers:

- Table-size rounding and zero-sentinel lookup/erase.
- High-bit IDs, common low halves with different high halves, and maximum IDs.
- Head, middle, and tail deletion in a cluster found by black-box probing of the
  actual table; the fixture does not duplicate the hash algorithm.
- All 720 deletion orders for a wrapped six-key cluster and a mixed-home cluster
  that must skip home-position entries before moving a displaced entry.
- 800,000 erase/insert cycles at the configured maximum population, including
  repeated erase, unrelated retained lookups, and full-table audits.
- 50,000 operations linking a real pool and index, checking that each ID resolves
  to its currently owned live slot after repeated release/reacquisition.

An independent `std::unordered_map` oracle checks every expected lookup. Physical
entry audits reject duplicate/unexpected IDs and require empty entries to have
`no_slot`; the implementation's invariant checker also runs at checkpoints.
Oracle containers and diagnostic checks run outside any measured hot path.

```sh
c++ -std=c++23 -O2 -Wall -Wextra -Wpedantic -Iinclude tests/unit/index_tests.cpp -o index_tests
./index_tests
```

These suites test component semantics and ownership, not benchmark timing. The
separate `memory_tests` target counts C++ allocations during book processing;
construction, oracle maintenance, and diagnostics are deliberately excluded.
