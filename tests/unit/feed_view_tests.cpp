#include "ledger/feed_view.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <ranges>
#include <stdexcept>
#include <vector>
#ifdef _WIN32
#include <malloc.h>
#endif

namespace {
thread_local bool track_allocations = false;
thread_local std::size_t allocation_count = 0;
void* allocate(std::size_t bytes) {
    if (track_allocations) ++allocation_count;
    if (auto* pointer = std::malloc(bytes ? bytes : 1)) return pointer;
    throw std::bad_alloc{};
}
void* allocate_aligned(std::size_t bytes, std::size_t alignment) {
    if (track_allocations) ++allocation_count;
    void* pointer{};
#ifdef _WIN32
    pointer = _aligned_malloc(bytes ? bytes : alignment, alignment);
    if (pointer) return pointer;
#else
    if (posix_memalign(&pointer, alignment, bytes ? bytes : alignment) == 0) return pointer;
#endif
    throw std::bad_alloc{};
}
void free_aligned(void* pointer) noexcept {
#ifdef _WIN32
    _aligned_free(pointer);
#else
    std::free(pointer);
#endif
}
}
void* operator new(std::size_t bytes) { return allocate(bytes); }
void* operator new[](std::size_t bytes) { return allocate(bytes); }
void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { std::free(pointer); }
void* operator new(std::size_t bytes, const std::nothrow_t&) noexcept { try { return allocate(bytes); } catch (...) { return nullptr; } }
void* operator new[](std::size_t bytes, const std::nothrow_t&) noexcept { try { return allocate(bytes); } catch (...) { return nullptr; } }
void operator delete(void* pointer, const std::nothrow_t&) noexcept { std::free(pointer); }
void operator delete[](void* pointer, const std::nothrow_t&) noexcept { std::free(pointer); }
void* operator new(std::size_t bytes, std::align_val_t alignment) { return allocate_aligned(bytes, static_cast<std::size_t>(alignment)); }
void* operator new[](std::size_t bytes, std::align_val_t alignment) { return allocate_aligned(bytes, static_cast<std::size_t>(alignment)); }
void operator delete(void* pointer, std::align_val_t) noexcept { free_aligned(pointer); }
void operator delete[](void* pointer, std::align_val_t) noexcept { free_aligned(pointer); }
void operator delete(void* pointer, std::size_t, std::align_val_t) noexcept { free_aligned(pointer); }
void operator delete[](void* pointer, std::size_t, std::align_val_t) noexcept { free_aligned(pointer); }
void* operator new(std::size_t bytes, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    try { return allocate_aligned(bytes, static_cast<std::size_t>(alignment)); } catch (...) { return nullptr; }
}
void* operator new[](std::size_t bytes, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    try { return allocate_aligned(bytes, static_cast<std::size_t>(alignment)); } catch (...) { return nullptr; }
}
void operator delete(void* pointer, std::align_val_t, const std::nothrow_t&) noexcept { free_aligned(pointer); }
void operator delete[](void* pointer, std::align_val_t, const std::nothrow_t&) noexcept { free_aligned(pointer); }

using namespace ledger;
namespace {
static_assert(std::forward_iterator<FeedView::Iterator>);
static_assert(std::ranges::forward_range<const FeedView>);
static_assert(noexcept(FeedView::parse({}, std::declval<FeedView&>())));
static_assert(noexcept(std::declval<const FeedView&>().read(0, std::declval<Command&>())));
static_assert(noexcept(*std::declval<FeedView::Iterator>()));

void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void put(std::span<std::byte> bytes, std::size_t offset, std::size_t width, std::uint64_t value) {
    for (std::size_t i = 0; i < width; ++i) bytes[offset + i] = std::byte((value >> (8 * i)) & 255);
}
std::vector<std::byte> fixture(std::span<const Command> commands) {
    std::vector<std::byte> bytes(feed_header_size + commands.size() * feed_record_size);
    constexpr std::array magic{'L', 'G', 'R', 'F', 'E', 'E', 'D', '\0'};
    for (std::size_t i = 0; i < magic.size(); ++i) bytes[i] = std::byte(magic[i]);
    put(bytes, 8, 2, feed_version); put(bytes, 10, 2, feed_header_size);
    put(bytes, 12, 2, feed_record_size); put(bytes, 16, 8, commands.size());
    auto destination = bytes.begin() + feed_header_size;
    for (const auto& command : commands) {
        const auto record = encode_record(command);
        destination = std::copy(record.begin(), record.end(), destination);
    }
    return bytes;
}
const std::array commands{
    Command{1, 14, 102, 6, Side::Buy, Action::Limit, TimeInForce::GTC},
    Command{2, 19, 0, 2, Side::Sell, Action::Market, TimeInForce::IOC},
    Command{3, 14, 104, 8, Side::Buy, Action::Replace, TimeInForce::FOK},
};

void test_valid_and_empty() {
    FeedView view;
    require(!view.valid() && view.empty() && view.size() == 0, "default view must be invalid and empty");
    require(view.begin() == view.end(), "invalid view is not an empty range");
    auto output = commands[0];
    require(!view.read(0, output) && output == commands[0], "invalid view changed output");

    const auto bytes = fixture(commands);
    require(FeedView::parse(bytes, view) && view.valid() && view.size() == commands.size(), "valid feed rejected");
    require(view.encoded_records().data() == bytes.data() + feed_header_size, "view copied its payload");
    require(std::ranges::equal(view, commands), "forward iteration changed command order");
    require(std::ranges::equal(view, commands), "second iteration differs");
    auto cursor = view.begin(); auto independent = cursor;
    require(*cursor++ == commands[0] && *cursor == commands[1], "postincrement did not return old command");
    require(*independent == commands[0], "iterator copy lost multipass position");
    for (std::size_t index = 0; index < commands.size(); ++index)
        require(view.read(index, output) && output == commands[index], "indexed access differs");
    require(!view.read(commands.size(), output) && output == commands.back(), "end index changes output");
    require(!view.read(std::numeric_limits<std::size_t>::max(), output), "maximum index accepted");

    const auto empty = fixture({});
    require(FeedView::parse(empty, view, 0) && view.valid() && view.empty(), "empty feed rejected");
    require(view.begin() == view.end() && !view.read(0, output), "empty feed exposed a record");
}

void test_framing_and_header() {
    const auto bytes = fixture(commands);
    FeedView retained;
    require(FeedView::parse(bytes, retained), "failed to build retained view");
    const auto pointer = retained.encoded_records().data();
    auto rejected = [&](std::span<const std::byte> candidate, std::uint64_t cap = 20'000'000) {
        require(!FeedView::parse(candidate, retained, cap), "malformed framing accepted");
        require(retained.valid() && retained.size() == commands.size()
                && retained.encoded_records().data() == pointer
                && std::ranges::equal(retained, commands), "failed parse replaced previous view");
    };
    for (std::size_t length = 0; length < bytes.size(); ++length)
        rejected(std::span(bytes).first(length));
    auto trailing = bytes; trailing.push_back(std::byte{0}); rejected(trailing);
    trailing = bytes; trailing.insert(trailing.end(), feed_record_size, std::byte{0}); rejected(trailing);
    for (const std::size_t offset : {0u, 7u, 8u, 9u, 10u, 11u, 12u, 13u, 14u, 15u,
                                     24u, 25u, 26u, 27u, 28u, 29u, 30u, 31u}) {
        auto malformed = bytes; malformed[offset] ^= std::byte{255}; rejected(malformed);
    }
    for (const std::uint64_t count : {std::uint64_t{0}, std::uint64_t{1}, std::uint64_t{4},
                                     std::uint64_t{20'000'001}, std::uint64_t{1} << 63,
                                     std::numeric_limits<std::uint64_t>::max()}) {
        auto malformed = bytes; put(malformed, 16, 8, count);
        rejected(malformed, std::numeric_limits<std::uint64_t>::max());
    }
    rejected(bytes, 0); rejected(bytes, commands.size() - 1);
    require(FeedView::parse(bytes, retained, commands.size()), "exact record cap rejected");
    auto empty_with_tail = fixture({}); empty_with_tail.push_back(std::byte{0}); rejected(empty_with_tail);
}

void test_all_record_validation() {
    const auto bytes = fixture(commands);
    FeedView retained;
    require(FeedView::parse(bytes, retained), "failed to create retained valid view");
    const auto original = retained.encoded_records();
    // A valid header and early records must never expose a partially parsed
    // feed when any later record has an invalid enum or nonzero reserved field.
    for (std::size_t record = 0; record < commands.size(); ++record) {
        for (std::size_t offset = 28; offset < feed_record_size; ++offset) {
            auto malformed = bytes;
            malformed[feed_header_size + record * feed_record_size + offset] = std::byte{255};
            require(!FeedView::parse(malformed, retained), "invalid record was accepted");
            require(retained.encoded_records().data() == original.data()
                    && retained.encoded_records().size() == original.size()
                    && std::ranges::equal(retained, commands), "late invalid record changed published view");
            FeedView invalid;
            require(!FeedView::parse(malformed, invalid) && !invalid.valid() && invalid.empty(),
                    "failure exposed records in default view");
        }
    }
    const std::array semantic_errors{
        Command{0, 0, -100, 0, Side::Buy, Action::Limit, TimeInForce::GTC},
        Command{0, 0, std::numeric_limits<Price>::max(), 0, Side::Sell, Action::Replace, TimeInForce::FOK},
    };
    const auto framed = fixture(semantic_errors);
    require(FeedView::parse(framed, retained) && std::ranges::equal(retained, semantic_errors),
            "framing preflight incorrectly rejected engine-semantic errors");
}

void test_golden_unaligned_and_allocations() {
    // Hand-written wire bytes are independent of the encoder under test.
    constexpr std::array<unsigned char, 72> golden{
        0x4c, 0x47, 0x52, 0x46, 0x45, 0x45, 0x44, 0x00,
        0x01, 0x00, 0x20, 0x00, 0x28, 0x00, 0x00, 0x00,
        0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80,
        0xff, 0xff, 0xff, 0xff, 0x01, 0x03, 0x02, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    std::vector<std::byte> owner(golden.size() + 1);
    for (std::size_t i = 0; i < golden.size(); ++i) owner[i + 1] = std::byte(golden[i]);
    const auto wire = std::span<const std::byte>(owner).subspan(1);
    const Command expected{0x0102030405060708ULL, std::numeric_limits<OrderId>::max(),
                           std::numeric_limits<Price>::min(), std::numeric_limits<Quantity>::max(),
                           Side::Sell, Action::Replace, TimeInForce::FOK};
    FeedView view;
    require(FeedView::parse(wire, view, 1) && *view.begin() == expected, "unaligned golden feed differs");
    require(std::ranges::equal(view.encoded_records(), encode_record(expected)), "golden wire roundtrip differs");
    auto detached_iterator = view.begin();
    const auto empty = fixture({});
    require(FeedView::parse(empty, view) && *detached_iterator == expected,
            "iterator depends on the view object's replacement");

    auto malformed = owner; malformed.back() = std::byte{1};
    const auto bad_wire = std::span<const std::byte>(malformed).subspan(1);
    const auto before = allocation_count;
    bool results_correct = true;
    track_allocations = true;
    for (std::size_t run = 0; run < 1000; ++run) {
        results_correct &= FeedView::parse(wire, view, 1);
        auto decoded = expected;
        results_correct &= view.read(0, decoded) && decoded == expected;
        results_correct &= !view.read(std::numeric_limits<std::size_t>::max(), decoded);
        results_correct &= !FeedView::parse(bad_wire, view);
        results_correct &= !FeedView::parse(wire.first(17), view);
        for (const auto command : view) results_correct &= command == expected;
    }
    track_allocations = false;
    require(results_correct, "allocation-free parser operations produced wrong results");
    require(allocation_count == before, "feed parsing or access requested C++ heap storage");
}
}

int main() {
    try {
        test_valid_and_empty();
        test_framing_and_header();
        test_all_record_validation();
        test_golden_unaligned_and_allocations();
        std::cout << "PASS feed views, hostile framing, atomicity, unaligned golden wire and 1000 allocation-free cycles\n";
        return 0;
    } catch (const std::exception& error) {
        track_allocations = false;
        std::cerr << "FAIL " << error.what() << '\n'; return 1;
    }
}
