#include "ledger/book.hpp"
#include "ledger/types.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
std::uint64_t number(std::string_view input) {
    std::uint64_t value{};
    const auto [end, error] = std::from_chars(input.data(), input.data() + input.size(), value);
    if (input.empty() || error != std::errc{} || end != input.data() + input.size())
        throw std::invalid_argument("expected an unsigned decimal integer");
    return value;
}

class Simulation {
    static constexpr ledger::OrderId maker_bid = 1, maker_ask = 2;
    static constexpr ledger::OrderId external_bid = 3, external_ask = 4;
public:
    Simulation(std::uint64_t steps, std::uint64_t seed, std::uint32_t quote_quantity,
               std::int64_t inventory_limit)
        : steps_(steps), seed_(seed), quote_quantity_(quote_quantity), limit_(inventory_limit),
          book_(ledger::Config{1, 20000, 16}), output_(storage_) {}

    void run(std::ostream* trace = nullptr) {
        using namespace ledger;
        std::mt19937_64 random(seed_);
        if (trace != nullptr) {
            *trace << "step,mid_ticks,inventory_units,cash_tick_units,pnl_tick_units,"
                "maker_filled_units,maker_trade_events,max_abs_inventory_units\n";
            write_trace(*trace, 0);
        }
        for (std::uint64_t step = 0; step < steps_; ++step) {
            // All live IDs are tracked from the actual engine execution reports.
            for (OrderId id = 1; id <= 4; ++id) {
                if (remaining_[id] != 0)
                    apply({++sequence_, id, 0, 0, id % 2 == 1 ? Side::Buy : Side::Sell,
                           Action::Cancel, TimeInForce::GTC});
            }
            const auto external_depth = static_cast<Quantity>(quote_quantity_ * 2);
            post(external_bid, Side::Buy, mid_ - 2, external_depth, false);
            post(external_ask, Side::Sell, mid_ + 2, external_depth, false);
            // Each side is independently capped at its remaining position room.
            const auto buy_size = static_cast<Quantity>(std::min<std::int64_t>(quote_quantity_, limit_ - inventory_));
            const auto sell_size = static_cast<Quantity>(std::min<std::int64_t>(quote_quantity_, limit_ + inventory_));
            if (buy_size != 0) post(maker_bid, Side::Buy, mid_ - 1, buy_size, true);
            if (sell_size != 0) post(maker_ask, Side::Sell, mid_ + 1, sell_size, true);

            const auto draw = random();
            const bool buyer = (draw & 1U) != 0;
            const auto quantity = static_cast<Quantity>(1 + ((draw >> 1) % (quote_quantity_ * 2)));
            const auto previous_fills = maker_filled_units_;
            apply({++sequence_, step + 5, 0, quantity, buyer ? Side::Buy : Side::Sell,
                   Action::Market, TimeInForce::IOC});
            ++flow_orders_; flow_units_ += quantity;
            if (maker_filled_units_ != previous_fills) ++flow_orders_with_maker_fill_;

            // Synthetic adverse selection: the fair mark moves 0..3 ticks in
            // the aggressor's direction after execution. No historical feed.
            const auto jump = static_cast<Price>((draw >> 32) % 4);
            mid_ = std::clamp<Price>(mid_ + (buyer ? jump : -jump), 1000, 19000);
            if (trace != nullptr) write_trace(*trace, step + 1);
        }
        for (ledger::OrderId id = 1; id <= 4; ++id)
            if (remaining_[id] != 0)
                apply({++sequence_, id, 0, 0, id % 2 == 1 ? ledger::Side::Buy : ledger::Side::Sell,
                       ledger::Action::Cancel, ledger::TimeInForce::GTC});
        if (trace != nullptr) trace->flush();
        const auto pnl = cash_ + inventory_ * mid_;
        if (max_abs_inventory_ > limit_) throw std::runtime_error("inventory limit violated");
        if (maker_buy_units_ - maker_sell_units_ != inventory_)
            throw std::runtime_error("execution inventory accounting mismatch");
        if (maker_filled_units_ != static_cast<std::uint64_t>(maker_buy_units_ + maker_sell_units_))
            throw std::runtime_error("execution volume accounting mismatch");
        std::cout << std::setprecision(10)
            << "{\n  \"scenario\": \"seeded_synthetic_passive_market_maker\",\n"
            << "  \"historical_backtest\": false,\n  \"live_trading\": false,\n"
            << "  \"fees_modeled\": false,\n  \"queue_position_model\": false,\n"
            << "  \"steps\": " << steps_ << ",\n  \"seed\": " << seed_
            << ",\n  \"quote_quantity\": " << quote_quantity_
            << ",\n  \"inventory_limit_units\": " << limit_
            << ",\n  \"max_abs_inventory_units\": " << max_abs_inventory_
            << ",\n  \"inventory_units\": " << inventory_
            << ",\n  \"cash_tick_units\": " << cash_
            << ",\n  \"final_mid_ticks\": " << mid_
            << ",\n  \"mark_to_market_pnl_tick_units\": " << pnl
            << ",\n  \"maker_buy_units\": " << maker_buy_units_
            << ",\n  \"maker_sell_units\": " << maker_sell_units_
            << ",\n  \"maker_trade_events\": " << maker_trade_events_
            << ",\n  \"maker_filled_units\": " << maker_filled_units_
            << ",\n  \"maker_quote_units_submitted\": " << maker_quoted_units_
            << ",\n  \"quote_unit_fill_rate\": " << ratio(maker_filled_units_, maker_quoted_units_)
            << ",\n  \"flow_orders_with_maker_fill\": " << flow_orders_with_maker_fill_
            << ",\n  \"flow_orders\": " << flow_orders_
            << ",\n  \"flow_order_maker_fill_rate\": " << ratio(flow_orders_with_maker_fill_, flow_orders_)
            << ",\n  \"flow_units\": " << flow_units_ << "\n}\n";
    }
private:
    void write_trace(std::ostream& trace, std::uint64_t step) const {
        trace << step << ',' << mid_ << ',' << inventory_ << ',' << cash_ << ','
              << cash_ + inventory_ * mid_ << ',' << maker_filled_units_ << ','
              << maker_trade_events_ << ',' << max_abs_inventory_ << '\n';
    }
    static double ratio(std::uint64_t numerator, std::uint64_t denominator) {
        return denominator == 0 ? 0 : static_cast<double>(numerator) / static_cast<double>(denominator);
    }
    void apply(const ledger::Command& command) {
        if (book_.process(command, output_) != ledger::ProcessResult::Applied)
            throw std::runtime_error("unexpected engine rejection in synthetic scenario");
        for (const auto& event : output_.events()) {
            if (event.type == ledger::EventType::Trade) {
                if (event.id == maker_bid || event.id == maker_ask)
                    throw std::runtime_error("maker quote crossed the external book");
                if (event.contra_id == maker_bid || event.contra_id == maker_ask) {
                    const auto quantity = static_cast<std::int64_t>(event.quantity);
                    const auto money = event.price * quantity;
                    if (event.contra_id == maker_bid) {
                        inventory_ += quantity; cash_ -= money; maker_buy_units_ += quantity;
                    } else {
                        inventory_ -= quantity; cash_ += money; maker_sell_units_ += quantity;
                    }
                    maker_filled_units_ += event.quantity; ++maker_trade_events_;
                    max_abs_inventory_ = std::max(max_abs_inventory_, std::abs(inventory_));
                    if (max_abs_inventory_ > limit_) throw std::runtime_error("inventory limit violated on fill");
                }
            }
            if (event.id >= 1 && event.id <= 4) {
                if (event.type == ledger::EventType::Filled || event.type == ledger::EventType::Cancelled)
                    remaining_[event.id] = 0;
                else if (event.type == ledger::EventType::PartiallyFilled)
                    remaining_[event.id] = event.remaining;
            }
        }
    }
    void post(ledger::OrderId id, ledger::Side side, ledger::Price price,
              ledger::Quantity quantity, bool maker) {
        apply({++sequence_, id, price, quantity, side, ledger::Action::Limit, ledger::TimeInForce::GTC});
        remaining_[id] = quantity; // Posting is required to be passive; apply checks trades.
        if (maker) maker_quoted_units_ += quantity;
    }

    std::uint64_t steps_, seed_;
    std::uint32_t quote_quantity_;
    std::int64_t limit_;
    ledger::FastBook book_;
    std::array<ledger::Event, ledger::required_event_capacity(16)> storage_{};
    ledger::EventBuffer output_;
    std::array<ledger::Quantity, 5> remaining_{};
    std::uint64_t sequence_{}, maker_filled_units_{}, maker_quoted_units_{}, maker_trade_events_{};
    std::uint64_t flow_orders_{}, flow_units_{}, flow_orders_with_maker_fill_{};
    std::int64_t inventory_{}, cash_{}, max_abs_inventory_{}, maker_buy_units_{}, maker_sell_units_{};
    ledger::Price mid_{10000};
};
}

int main(int argc, char** argv) {
    try {
        std::uint64_t steps = 10000, seed = 42, quote_quantity = 5, inventory_limit = 20;
        bool seen_steps = false, seen_seed = false, seen_quantity = false, seen_limit = false;
        bool seen_trace = false;
        std::string trace_path;
        if (argc == 2 && std::string_view(argv[1]) == "--help") {
            std::cout << "Usage: market_maker [--steps 1..1000000] [--seed uint64] "
                "[--quote-quantity 1..10000] [--inventory-limit 1..100000] [--trace FILE]\n";
            return 0;
        }
        for (int i = 1; i < argc; ++i) {
            const std::string_view option(argv[i]);
            if (i + 1 == argc) throw std::invalid_argument("missing option value");
            if (option == "--steps" && !seen_steps) { steps = number(argv[++i]); seen_steps = true; }
            else if (option == "--seed" && !seen_seed) { seed = number(argv[++i]); seen_seed = true; }
            else if (option == "--quote-quantity" && !seen_quantity) { quote_quantity = number(argv[++i]); seen_quantity = true; }
            else if (option == "--inventory-limit" && !seen_limit) { inventory_limit = number(argv[++i]); seen_limit = true; }
            else if (option == "--trace" && !seen_trace) { trace_path = argv[++i]; seen_trace = true; }
            else throw std::invalid_argument("unknown or duplicate option");
        }
        if (steps == 0 || steps > 1000000 || quote_quantity == 0 || quote_quantity > 10000
            || inventory_limit == 0 || inventory_limit > 100000)
            throw std::invalid_argument("option value out of range");
        std::ofstream trace;
        if (seen_trace) {
            if (trace_path.empty()) throw std::invalid_argument("trace path must not be empty");
            trace.exceptions(std::ios::failbit | std::ios::badbit);
            trace.open(trace_path);
        }
        Simulation(steps, seed, static_cast<std::uint32_t>(quote_quantity),
                   static_cast<std::int64_t>(inventory_limit)).run(seen_trace ? &trace : nullptr);
        if (seen_trace) trace.close();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "market_maker: " << error.what() << '\n'; return 2;
    }
}
