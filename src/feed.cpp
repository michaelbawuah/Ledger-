#include "ledger/feed.hpp"
#include <algorithm>
#include <bit>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>

namespace ledger {
namespace {
constexpr std::array<std::byte, 8> magic{
    std::byte{'L'}, std::byte{'G'}, std::byte{'R'}, std::byte{'F'},
    std::byte{'E'}, std::byte{'E'}, std::byte{'D'}, std::byte{0}};
std::uint64_t get(std::span<const std::byte> b, std::size_t pos, std::size_t n) noexcept {
    std::uint64_t value{};
    for (std::size_t i = 0; i < n; ++i)
        value |= static_cast<std::uint64_t>(std::to_integer<unsigned char>(b[pos+i])) << (8*i);
    return value;
}
void put(std::span<std::byte> b, std::size_t pos, std::size_t n, std::uint64_t value) noexcept {
    for (std::size_t i = 0; i < n; ++i) b[pos+i] = std::byte((value >> (8*i)) & 0xff);
}
bool enums_valid(const Command& c) noexcept {
    return static_cast<unsigned>(c.side) <= static_cast<unsigned>(Side::Sell)
        && static_cast<unsigned>(c.action) <= static_cast<unsigned>(Action::Replace)
        && static_cast<unsigned>(c.tif) <= static_cast<unsigned>(TimeInForce::FOK);
}
void read_exact(std::istream& in, std::span<std::byte> bytes) {
    in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!in) throw std::runtime_error("truncated feed");
}
void write_exact(std::ostream& out, std::span<const std::byte> bytes) {
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!out) throw std::runtime_error("failed to write feed");
}
}
bool decode_header(std::span<const std::byte> bytes, FeedHeader& out, std::uint64_t max_records) noexcept {
    if (bytes.size() != feed_header_size || !std::equal(magic.begin(), magic.end(), bytes.begin())) return false;
    if (get(bytes, 8, 2) != feed_version || get(bytes, 10, 2) != feed_header_size
        || get(bytes, 12, 2) != feed_record_size || get(bytes, 14, 2) != 0 || get(bytes, 24, 8) != 0) return false;
    const auto count = get(bytes, 16, 8);
    if (count > max_records || count > std::numeric_limits<std::size_t>::max() / sizeof(Command)
        || count > (std::numeric_limits<std::uint64_t>::max() - feed_header_size) / feed_record_size) return false;
    out.record_count = count; return true;
}
EncodedRecord encode_record(const Command& c) {
    if (!enums_valid(c)) throw std::invalid_argument("cannot encode invalid enum value");
    EncodedRecord bytes{};
    put(bytes, 0, 8, c.sequence); put(bytes, 8, 8, c.id);
    put(bytes, 16, 8, std::bit_cast<std::uint64_t>(c.price)); put(bytes, 24, 4, c.quantity);
    bytes[28] = std::byte(static_cast<unsigned char>(c.side));
    bytes[29] = std::byte(static_cast<unsigned char>(c.action));
    bytes[30] = std::byte(static_cast<unsigned char>(c.tif));
    return bytes;
}
bool decode_record(std::span<const std::byte> b, Command& out) noexcept {
    if (b.size() != feed_record_size || b[31] != std::byte{0} || get(b, 32, 8) != 0) return false;
    Command c;
    c.sequence = get(b, 0, 8); c.id = get(b, 8, 8);
    c.price = std::bit_cast<std::int64_t>(get(b, 16, 8));
    c.quantity = static_cast<Quantity>(get(b, 24, 4));
    c.side = static_cast<Side>(std::to_integer<unsigned char>(b[28]));
    c.action = static_cast<Action>(std::to_integer<unsigned char>(b[29]));
    c.tif = static_cast<TimeInForce>(std::to_integer<unsigned char>(b[30]));
    if (!enums_valid(c)) return false;
    out = c; return true;
}
std::vector<Command> read_commands(const std::filesystem::path& path, std::uint64_t max_records) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open feed: " + path.string());
    std::array<std::byte, feed_header_size> header{}; read_exact(in, header);
    FeedHeader decoded_header;
    if (!decode_header(header, decoded_header, max_records)) throw std::runtime_error("invalid, unsupported, or oversized feed header");
    const auto count = decoded_header.record_count;
    const auto expected_size = feed_header_size + count * feed_record_size;
    if (std::filesystem::file_size(path) != expected_size) throw std::runtime_error("feed size does not match record count");
    std::vector<Command> commands(static_cast<std::size_t>(count)); EncodedRecord bytes{};
    for (auto& c : commands) {
        read_exact(in, bytes);
        if (!decode_record(bytes, c)) throw std::runtime_error("invalid feed record");
    }
    if (in.peek() != std::char_traits<char>::eof()) throw std::runtime_error("trailing feed data");
    return commands;
}
void write_commands(const std::filesystem::path& path, std::span<const Command> commands) {
    std::array<std::byte, feed_header_size> header{};
    std::copy(magic.begin(), magic.end(), header.begin());
    put(header, 8, 2, feed_version); put(header, 10, 2, feed_header_size);
    put(header, 12, 2, feed_record_size); put(header, 16, 8, commands.size());
    // Validate before opening/truncating a destination.
    for (const auto& c : commands) if (!enums_valid(c)) throw std::invalid_argument("invalid feed enum value");
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("cannot create feed: " + path.string());
    write_exact(out, header);
    for (const auto& c : commands) write_exact(out, encode_record(c));
    out.close();
    if (!out) throw std::runtime_error("failed to close feed");
}
}
