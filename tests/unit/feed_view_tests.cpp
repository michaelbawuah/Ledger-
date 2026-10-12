#include "ledger/feed_view.hpp"

#include <algorithm>
#include <array>
#include <iostream>
#include <limits>
#include <ranges>
#include <stdexcept>
#include <vector>

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
}

int main() {
    try {
        test_valid_and_empty();
        std::cout << "PASS borrowed feed view, indexed decode, forward iteration and empty feeds\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n'; return 1;
    }
}
