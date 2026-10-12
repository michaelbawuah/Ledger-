# Binary feed and borrowed views

Ledger's version 1 feed uses an explicit little-endian wire format. `Command`
objects are decoded field by field; native struct sizes, alignment, padding, and
host byte order do not define the file format. Prices are signed 64-bit integer
ticks, represented by their two's-complement bits. Quantity is an unsigned
32-bit integer. A feed contains one 32-byte header followed by exactly the
declared number of 40-byte records; zero records is valid.

## Header

| Offset | Bytes | Field | Required value |
| --- | --- | --- | --- |
| 0 | 8 | Magic | `4c 47 52 46 45 45 44 00` (`LGRFEED` plus NUL) |
| 8 | 2 | Version | 1 |
| 10 | 2 | Header size | 32 |
| 12 | 2 | Record size | 40 |
| 14 | 2 | Reserved | 0 |
| 16 | 8 | Record count | Nonnegative unsigned count, within caller's cap |
| 24 | 8 | Reserved | 0 |

## Record

Offsets here are relative to the start of each record.

| Offset | Bytes | Field | Encoding |
| --- | --- | --- | --- |
| 0 | 8 | Sequence | Unsigned 64-bit integer |
| 8 | 8 | Order ID | Unsigned 64-bit integer |
| 16 | 8 | Price | Signed 64-bit integer tick value |
| 24 | 4 | Quantity | Unsigned 32-bit integer |
| 28 | 1 | Side | 0 Buy, 1 Sell |
| 29 | 1 | Action | 0 Limit, 1 Market, 2 Cancel, 3 Replace |
| 30 | 1 | Time in force | 0 GTC, 1 IOC, 2 FOK |
| 31 | 1 | Reserved | 0 |
| 32 | 8 | Reserved | 0 |

The decoders reject unknown versions, invalid enum values, nonzero reserved
fields, and incorrect framing. Engine semantics are a separate step: zero IDs,
zero quantities, prices outside the configured domain, duplicate IDs, and
sequence handling are not validated by the wire parser. This lets a replay
preserve rejected commands for engine testing.

## Preflight without a command vector

`FeedView::parse(bytes, out, max_records)` borrows a `std::span<const std::byte>`.
It validates the header, its record cap and representability limits, the exact
payload size, and every record before publishing `out`. Both short input and
trailing bytes fail. Payload division and remainder checks avoid multiplying an
untrusted count. A corrupt final record fails the entire parse; no partial feed
is published. Failure leaves an existing `out` unchanged.

The default cap is 20,000,000 records. A lower application-specific cap can be
passed explicitly. Raising it does not bypass the shared header decoder's
integer representability limits. The view imposes no new allocation or ownership
of the feed bytes. It can borrow a vector, a memory-mapped file, or another live
contiguous byte buffer, including storage at an unaligned address.

```cpp
#include "ledger/feed_view.hpp"

// The caller owns wire and keeps it alive and unchanged during this call.
bool replay_bytes(std::span<const std::byte> wire) {
    ledger::FeedView view;
    if (!ledger::FeedView::parse(wire, view, 1'000'000)) return false;
    for (const ledger::Command command : view) {
        // Submit command to the matching engine here.
        (void)command;
    }
    return true;
}
```

Initial preflight is O(number of records); checked indexed access and each
iterator step are O(1). Parsing, `read()`, and iteration allocate no C++ heap
storage and are `noexcept`. Each access decodes one `Command` value; zero-copy
means the feed bytes are borrowed, not that wire bytes are reinterpreted as
native `Command` objects. `encoded_records()` exposes the borrowed payload,
excluding the header.

## Lifetime and failure contract

- A default view is invalid, empty, and has equal begin/end iterators. A
  successfully parsed zero-record feed is valid and empty. Use `valid()` when
  that distinction matters.
- `read(index, command)` returns false for invalid views or out-of-range indices
  and leaves `command` unchanged. A failed record decode also leaves it unchanged.
- The byte owner must outlive every copied view and iterator. Moving or resizing
  a vector, unmapping a file, freeing a buffer, or changing its bytes invalidates
  all dependent views and iterators. A `const` span does not prevent writes
  through another alias. No post-preflight mutation is supported.
- Iterators decode commands by value and satisfy forward-iterator requirements.
  Copies keep independent positions. Replacing or destroying the `FeedView`
  object alone does not invalidate an iterator: it borrows the owner's bytes
  directly. Dereferencing end or incrementing past end violates the iterator
  contract. Use `read()` for checked access.
- Independent readers may share immutable live storage. The view does not add
  synchronization or extend the owner's lifetime.

## Verification and fuzz seeds

`tests/unit/feed_view_tests.cpp` checks empty/default views, safe indexing,
forward-iterator multipass behavior, exact record caps, every prefix truncation,
trailing bytes, hostile 64-bit counts, header corruption, invalid enums and
reserved bytes at every record position, late-record failure atomicity, and
independent golden wire bytes with extreme signed/unsigned values. It also
counts C++ heap requests across 1,000 parse/access/iteration cycles, including
invalid input paths, and verifies unaligned storage.

The test can be built without additional dependencies:

```sh
g++ -std=c++23 -Wall -Wextra -Wpedantic -Iinclude \
    src/feed.cpp src/feed_view.cpp tests/unit/feed_view_tests.cpp \
    -o /tmp/ledger-feed-view-tests
/tmp/ledger-feed-view-tests
```

`tools/make_fuzz_corpus.py` writes deterministic valid and malformed record,
header, and whole-feed inputs into a specified seed directory. It preserves
unrelated files already in that directory. These are wire-format seeds, with no
native layouts or external market data.
