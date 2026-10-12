#include "ledger/book.hpp"
#include "ledger/feed.hpp"
#include "ledger/reference_book.hpp"
#include <bit>
#include <charconv>
#include <fstream>
#include <iostream>
#include <string>
#include <unordered_set>

namespace {
template<class T> T number(const std::string& value, const std::string& name) {
    T out{}; const auto [end, ec] = std::from_chars(value.data(), value.data()+value.size(), out);
    if (ec != std::errc{} || end != value.data()+value.size()) throw std::invalid_argument("invalid " + name);
    return out;
}
void mix(std::uint64_t& sum, std::uint64_t value) { sum ^= value; sum *= 1099511628211ULL; }
template<class Book> void run(Book& book, std::span<const ledger::Command> commands,
                              ledger::Config config, const std::string& events_path) {
    std::vector<ledger::Event> storage(ledger::required_event_capacity(config.max_orders));
    ledger::EventBuffer events(storage); std::ofstream output;
    if (!events_path.empty()) {
        output.open(events_path);
        if (!output) throw std::runtime_error("cannot create event CSV");
        output << "sequence,type,reason,id,contra_id,side,price,quantity,remaining\n";
    }
    std::uint64_t checksum = 14695981039346656037ULL, rejected{};
    for (const auto& command : commands) {
        events.clear(); const auto result = book.process(command, events);
        if (result == ledger::ProcessResult::OutputFull) throw std::runtime_error("event buffer unexpectedly full");
        if (result == ledger::ProcessResult::Rejected) ++rejected;
        for (const auto& e : events.events()) {
            mix(checksum, e.sequence); mix(checksum, static_cast<unsigned>(e.type));
            mix(checksum, static_cast<unsigned>(e.reason)); mix(checksum, e.id); mix(checksum, e.contra_id);
            mix(checksum, static_cast<unsigned>(e.side)); mix(checksum, std::bit_cast<std::uint64_t>(e.price));
            mix(checksum, e.quantity); mix(checksum, e.remaining);
            if (output) output << e.sequence << ',' << static_cast<unsigned>(e.type) << ','
                << static_cast<unsigned>(e.reason) << ',' << e.id << ',' << e.contra_id << ','
                << static_cast<unsigned>(e.side) << ',' << e.price << ',' << e.quantity << ',' << e.remaining << '\n';
        }
    }
    if (output.is_open()) { output.close(); if (!output) throw std::runtime_error("failed to write event CSV"); }
    if (!book.check_invariants()) throw std::runtime_error("book invariant failure after replay");
    const auto& s = book.stats();
    std::cout << "{\"records\":" << commands.size() << ",\"rejected_commands\":" << rejected
              << ",\"live_orders\":" << book.size() << ",\"best_bid\":" << book.best_bid()
              << ",\"best_ask\":" << book.best_ask() << ",\"trade_count\":" << s.trade_count
              << ",\"executed_quantity\":" << s.executed << ",\"event_checksum\":" << checksum << "}\n";
}
}
int main(int argc, char** argv) {
    try {
        ledger::Config config; std::string input, events, engine = "fast-bitmap";
        std::unordered_set<std::string> seen;
        for (int i = 1; i < argc; ++i) {
            const std::string option = argv[i];
            if (option == "--help") {
                std::cout << "replay --input FILE [--engine reference|fast-bitmap|fast-linear] [--events FILE] [--min-price N] [--max-price N] [--capacity N]\n";
                return 0;
            }
            if (!seen.insert(option).second) throw std::invalid_argument("duplicate option: " + option);
            if (i + 1 == argc) throw std::invalid_argument("missing value for " + option);
            const std::string value = argv[++i];
            if (option == "--input") input = value;
            else if (option == "--events") events = value;
            else if (option == "--engine") engine = value;
            else if (option == "--min-price") config.min_price = number<ledger::Price>(value, option);
            else if (option == "--max-price") config.max_price = number<ledger::Price>(value, option);
            else if (option == "--capacity") config.max_orders = number<std::uint32_t>(value, option);
            else throw std::invalid_argument("unknown option: " + option);
        }
        if (input.empty()) throw std::invalid_argument("--input is required");
        if (config.min_price <= 0 || config.max_price < config.min_price
            || config.max_price - config.min_price > 999999 || !config.max_orders || config.max_orders > 1'000'000)
            throw std::invalid_argument("price domain must contain 1..1000000 positive ticks; capacity must be 1..1000000");
        const auto commands = ledger::read_commands(input, 5'000'000);
        if (engine == "reference") { ledger::ReferenceBook book(config); run(book, commands, config, events); }
        else if (engine == "fast-bitmap" || engine == "fast-linear") {
            ledger::FastBook book(config, engine == "fast-bitmap"); run(book, commands, config, events);
        } else throw std::invalid_argument("unknown engine: " + engine);
        return 0;
    } catch (const std::exception& e) { std::cerr << "replay: " << e.what() << '\n'; return 1; }
}
