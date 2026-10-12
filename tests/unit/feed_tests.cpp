#include "ledger/feed.hpp"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>

using namespace ledger;
namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class Function> void rejects(Function function) {
    bool failed = false;
    try { function(); } catch (const std::exception&) { failed = true; }
    require(failed, "malformed feed was accepted");
}
void bytes_to_file(const std::filesystem::path& path, const std::vector<char>& bytes) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    require(bool(stream), "failed writing test file");
}
}
int main() {
    const auto directory = std::filesystem::temp_directory_path() / ("ledger-feed-test-" + std::to_string(std::random_device{}()));
    try {
        std::filesystem::create_directory(directory);
        const auto file = directory / "test.lob";
        std::vector<Command> commands{
            {0x0102030405060708ULL, std::numeric_limits<OrderId>::max(), std::numeric_limits<Price>::min(), std::numeric_limits<Quantity>::max(), Side::Sell, Action::Replace, TimeInForce::FOK},
            {2, 1, 100, 4, Side::Buy, Action::Limit, TimeInForce::GTC},
            {3, 1, 0, 0, Side::Buy, Action::Cancel, TimeInForce::IOC},
        };
        for (const auto& command : commands) {
            Command decoded{};
            require(decode_record(encode_record(command), decoded) && decoded == command, "record roundtrip differs");
        }
        auto encoded = encode_record(commands.front());
        require(encoded[0] == std::byte{8} && encoded[7] == std::byte{1}, "sequence is not explicit little endian");
        require(encoded[23] == std::byte{0x80} && encoded[16] == std::byte{0}, "signed price wire bits differ");
        for (std::size_t length = 0; length < feed_record_size; ++length) {
            auto output = commands[1];
            require(!decode_record(std::span(encoded).first(length), output) && output == commands[1], "truncation changes output");
        }
        for (std::size_t offset = 28; offset < feed_record_size; ++offset) {
            auto mutated = encoded; mutated[offset] = std::byte{255};
            auto output = commands[1];
            require(!decode_record(mutated, output) && output == commands[1], "invalid enum/reserved byte changes output");
        }
        write_commands(file, commands);
        require(read_commands(file) == commands, "file replay differs");
        rejects([&] { (void)read_commands(file, 2); });
        std::ifstream stream(file, std::ios::binary);
        const std::vector<char> original{std::istreambuf_iterator<char>(stream), {}};
        for (const std::size_t offset : {0u, 8u, 10u, 12u, 14u, 24u}) {
            auto bytes = original; bytes[offset] ^= char(127); bytes_to_file(file, bytes);
            rejects([&] { (void)read_commands(file); });
        }
        auto malformed = original; std::fill(malformed.begin() + 16, malformed.begin() + 24, char(255));
        bytes_to_file(file, malformed); rejects([&] { (void)read_commands(file); });
        malformed = original; malformed.pop_back(); bytes_to_file(file, malformed);
        rejects([&] { (void)read_commands(file); });
        malformed = original; malformed.push_back(0); bytes_to_file(file, malformed);
        rejects([&] { (void)read_commands(file); });
        for (std::size_t count = 0; count < feed_header_size; ++count) {
            bytes_to_file(file, std::vector<char>(original.begin(), original.begin() + count));
            rejects([&] { (void)read_commands(file); });
        }
        write_commands(file, {}); require(read_commands(file).empty(), "empty feed roundtrip");
        auto invalid = commands; invalid.front().side = static_cast<Side>(255);
        rejects([&] { write_commands(file, invalid); });
        require(read_commands(file).empty(), "invalid write truncated destination");
        std::mt19937_64 random(73);
        for (std::size_t i = 0; i < 50000; ++i) {
            auto mutated = encode_record(commands[i % commands.size()]);
            mutated[random() % mutated.size()] = std::byte(random() & 255);
            Command decoded{};
            if (decode_record(mutated, decoded)) require(encode_record(decoded) == mutated, "accepted record isn't canonical");
        }
        std::filesystem::remove_all(directory);
        std::cout << "PASS feed roundtrip, endian golden bytes, malformed headers/records, output atomicity, 50000 seeded mutations\n";
        return 0;
    } catch (const std::exception& error) {
        std::filesystem::remove_all(directory);
        std::cerr << "FAIL " << error.what() << '\n'; return 1;
    }
}
