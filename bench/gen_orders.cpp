#include "ledger/feed.hpp"
#include "ledger/reference_book.hpp"
#include "ledger/workload.hpp"
#include <charconv>
#include <iostream>
#include <string>
#include <unordered_set>

namespace {
template<class T> T number(const std::string& value, const std::string& name) {
    T out{}; const auto [end, ec] = std::from_chars(value.data(), value.data()+value.size(), out);
    if (ec != std::errc{} || end != value.data()+value.size()) throw std::invalid_argument("invalid " + name);
    return out;
}
}
int main(int argc, char** argv) {
    try {
        std::size_t count = 1'000'000; std::uint64_t seed = 20261011;
        ledger::Config config; std::string path = "orders.ledger";
        std::unordered_set<std::string> seen;
        for (int i = 1; i < argc; ++i) {
            const std::string option = argv[i];
            if (option == "--help") {
                std::cout << "gen_orders [--out FILE] [--commands N] [--seed N] [--min-price N] [--max-price N] [--capacity N]\n";
                return 0;
            }
            if (!seen.insert(option).second) throw std::invalid_argument("duplicate option: " + option);
            if (i + 1 == argc) throw std::invalid_argument("missing value for " + option);
            const std::string value = argv[++i];
            if (option == "--out") path = value;
            else if (option == "--commands") count = number<std::size_t>(value, option);
            else if (option == "--seed") seed = number<std::uint64_t>(value, option);
            else if (option == "--min-price") config.min_price = number<ledger::Price>(value, option);
            else if (option == "--max-price") config.max_price = number<ledger::Price>(value, option);
            else if (option == "--capacity") config.max_orders = number<std::uint32_t>(value, option);
            else throw std::invalid_argument("unknown option: " + option);
        }
        if (!count || count > 5'000'000) throw std::invalid_argument("commands must be in [1, 5000000]");
        if (path.empty()) throw std::invalid_argument("empty output path");
        if (config.min_price <= 0 || config.max_price < config.min_price
            || config.max_price - config.min_price > 999999 || !config.max_orders || config.max_orders > 1'000'000)
            throw std::invalid_argument("price domain must contain 1..1000000 positive ticks; capacity must be 1..1000000");
        ledger::WorkloadGenerator<ledger::ReferenceBook> generator(seed, config);
        std::vector<ledger::Command> commands; commands.reserve(count);
        for (std::size_t i = 0; i < count; ++i) commands.push_back(generator.next());
        ledger::write_commands(path, commands);
        const auto& s = generator.stats();
        std::cout << "{\"records\":" << count << ",\"seed\":" << seed
                  << ",\"limits\":" << s.limits << ",\"cancels\":" << s.cancels
                  << ",\"markets\":" << s.markets << ",\"rejected_commands\":" << s.rejected_commands
                  << ",\"empty_book_cancels\":" << s.empty_book_cancels
                  << ",\"capacity_ioc_limits\":" << s.capacity_ioc_limits
                  << ",\"peak_live_orders\":" << s.peak_live_orders << "}\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "gen_orders: " << e.what() << '\n'; return 1;
    }
}
