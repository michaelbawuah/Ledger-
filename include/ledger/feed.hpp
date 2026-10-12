#pragma once
#include "ledger/types.hpp"
#include <array>
#include <filesystem>
#include <span>
#include <vector>

namespace ledger {
// Wire format is fixed, explicit little endian; native object layouts are never written.
inline constexpr std::size_t feed_header_size = 32;
inline constexpr std::size_t feed_record_size = 40;
inline constexpr std::uint16_t feed_version = 1;
using EncodedRecord = std::array<std::byte, feed_record_size>;
struct FeedHeader { std::uint64_t record_count{}; };
bool decode_header(std::span<const std::byte> bytes, FeedHeader& out,
                   std::uint64_t max_records = 20'000'000) noexcept;
EncodedRecord encode_record(const Command& command);
// Invalid framing, reserved fields, or enum values return false without changing out.
// Semantic errors (zero ID/quantity, out-of-domain price) are left to the engine.
bool decode_record(std::span<const std::byte> bytes, Command& out) noexcept;
std::vector<Command> read_commands(const std::filesystem::path& path,
                                   std::uint64_t max_records = 20'000'000);
void write_commands(const std::filesystem::path& path, std::span<const Command> commands);
}
