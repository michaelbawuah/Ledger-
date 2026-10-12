#include "ledger/feed.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <span>

namespace {
void check(bool valid) noexcept { if (!valid) std::abort(); }
void check_record(std::span<const std::byte> input) {
    const ledger::Command sentinel{0xfab1a98ced773921ULL, 0xa74103e22f617cc1ULL, -7482, 9481,
                                   ledger::Side::Sell, ledger::Action::Replace, ledger::TimeInForce::FOK};
    auto decoded = sentinel;
    const auto success = ledger::decode_record(input, decoded);
    if (!success) { check(decoded == sentinel); return; }
    check(input.size() == ledger::feed_record_size);
    check(static_cast<unsigned>(decoded.side) <= static_cast<unsigned>(ledger::Side::Sell));
    check(static_cast<unsigned>(decoded.action) <= static_cast<unsigned>(ledger::Action::Replace));
    check(static_cast<unsigned>(decoded.tif) <= static_cast<unsigned>(ledger::TimeInForce::FOK));
    const auto encoded = ledger::encode_record(decoded);
    check(std::equal(encoded.begin(), encoded.end(), input.begin()));
    ledger::Command roundtrip{}; check(ledger::decode_record(encoded, roundtrip)); check(roundtrip == decoded);
}
void check_header(std::span<const std::byte> input, std::uint64_t limit) {
    constexpr auto sentinel = std::numeric_limits<std::uint64_t>::max() - 73;
    ledger::FeedHeader header{sentinel};
    const auto success = ledger::decode_header(input, header, limit);
    if (!success) { check(header.record_count == sentinel); return; }
    check(input.size() == ledger::feed_header_size); check(header.record_count <= limit);
}
}

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const auto input = std::span{reinterpret_cast<const std::byte*>(data), size};
    // Exact framing, truncated/overlong inputs, enums, reserved bytes and rejected
    // decoder output atomicity are checked independently of engine validation.
    check_record(input); check_header(input, 20'000'000);
    if (size >= ledger::feed_header_size) {
        const auto header_bytes = input.first(ledger::feed_header_size);
        check_header(header_bytes, 0); check_header(header_bytes, 1024); check_header(header_bytes, 20'000'000);
        auto payload = input.subspan(ledger::feed_header_size);
        // No record-count-driven allocation or multiplication: hostile counts
        // cannot turn a small fuzzer input into an unbounded parser operation.
        for (std::size_t count = 0; !payload.empty() && count < 256; ++count) {
            const auto length = std::min(payload.size(), ledger::feed_record_size);
            check_record(payload.first(length)); payload = payload.subspan(length);
        }
    }
    return 0;
}
