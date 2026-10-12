#include "ledger/book.hpp"
#include "ledger/reference_book.hpp"

#include <array>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#ifdef LEDGER_USE_GTEST
#include <gtest/gtest.h>
#endif

namespace {
using namespace ledger;
void require(bool value, const char* expression, int line) {
    if (!value) throw std::runtime_error(std::string("line ") + std::to_string(line) + ": " + expression);
}
#define CHECK(...) require(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

template<class Book> struct Fixture {
    Config config{1, 200, 16};
    Book book{config};
    std::array<Event, required_event_capacity(16)> storage{};
    EventBuffer output{storage};
    std::uint64_t sequence{1};
    Command limit(OrderId id, Side side, Price price, Quantity quantity, TimeInForce tif = TimeInForce::GTC) {
        return {sequence++, id, price, quantity, side, Action::Limit, tif};
    }
    Command cancel(OrderId id, Side side) { return {sequence++, id, 0, 0, side, Action::Cancel, TimeInForce::GTC}; }
    Command replace(OrderId id, Side side, Price price, Quantity quantity, TimeInForce tif = TimeInForce::GTC) {
        return {sequence++, id, price, quantity, side, Action::Replace, tif};
    }
    Command market(OrderId id, Side side, Quantity quantity, TimeInForce tif = TimeInForce::GTC) {
        return {sequence++, id, 0, quantity, side, Action::Market, tif};
    }
    std::vector<Event> run(Command command, ProcessResult expected = ProcessResult::Applied) {
        CHECK(book.process(command, output) == expected);
        CHECK(book.check_invariants());
        const auto& stats = book.stats();
        CHECK(stats.submitted == 2 * stats.executed + stats.cancelled + stats.expired + book.resting_quantity());
        for (const auto& event : output.events()) CHECK(event.sequence == command.sequence);
        return {output.events().begin(), output.events().end()};
    }
    void reject(Command command, RejectReason reason) {
        auto events = run(command, ProcessResult::Rejected);
        CHECK(events.size() == 1);
        CHECK(events.front().type == EventType::Rejected);
        CHECK(events.front().reason == reason);
    }
    std::vector<Event> trades(const std::vector<Event>& events) {
        std::vector<Event> selected;
        for (auto event : events) if (event.type == EventType::Trade) selected.push_back(event);
        return selected;
    }
};

template<class B> void fifo_same_price() {
    Fixture<B> f;
    f.run(f.limit(1, Side::Sell, 100, 10)); f.run(f.limit(2, Side::Sell, 100, 10));
    auto events = f.run(f.limit(3, Side::Buy, 100, 15)); auto trades = f.trades(events);
    CHECK(trades.size() == 2); CHECK(trades[0].contra_id == 1); CHECK(trades[1].contra_id == 2);
    CHECK(trades[0].quantity == 10); CHECK(trades[1].quantity == 5);
    CHECK(f.book.snapshot().orders == std::vector<RestingOrder>{{2, Side::Sell, 100, 5}});
}
template<class B> void best_price_before_time() {
    Fixture<B> f;
    f.run(f.limit(1, Side::Sell, 110, 10)); f.run(f.limit(2, Side::Sell, 100, 10)); f.run(f.limit(3, Side::Sell, 105, 10));
    auto trades = f.trades(f.run(f.limit(4, Side::Buy, 110, 15)));
    CHECK(trades.size() == 2); CHECK(trades[0].contra_id == 2); CHECK(trades[1].contra_id == 3);
    CHECK(trades[0].price == 100); CHECK(trades[1].price == 105);
}
template<class B> void sell_taker_best_bid() {
    Fixture<B> f;
    f.run(f.limit(1, Side::Buy, 80, 10)); f.run(f.limit(2, Side::Buy, 90, 10));
    auto trades = f.trades(f.run(f.limit(3, Side::Sell, 75, 15)));
    CHECK(trades.size() == 2); CHECK(trades[0].contra_id == 2); CHECK(trades[0].price == 90); CHECK(trades[1].price == 80);
}
template<class B> void partial_fill_resting_residual() {
    Fixture<B> f; f.run(f.limit(1, Side::Sell, 100, 5));
    auto events = f.run(f.limit(2, Side::Buy, 110, 8));
    CHECK(events.size() == 4); CHECK(events[0].type == EventType::Accepted);
    CHECK(events[1].type == EventType::Trade); CHECK(events[2].type == EventType::Filled);
    CHECK(events[2].id == 1); CHECK(events[3].type == EventType::PartiallyFilled); CHECK(events[3].remaining == 3);
    CHECK(f.book.snapshot().orders == std::vector<RestingOrder>{{2, Side::Buy, 110, 3}});
}
template<class B> void partial_maker_event_order() {
    Fixture<B> f; f.run(f.limit(1, Side::Sell, 100, 10));
    auto events = f.run(f.limit(2, Side::Buy, 100, 3));
    CHECK(events.size() == 4); CHECK(events[1].type == EventType::Trade);
    CHECK(events[2].type == EventType::PartiallyFilled); CHECK(events[2].id == 1); CHECK(events[2].remaining == 7);
    CHECK(events[3].type == EventType::Filled); CHECK(events[3].id == 2); CHECK(events[3].remaining == 0);
}
template<class B> void cancel_position(std::size_t position) {
    Fixture<B> f;
    for (OrderId id = 1; id <= 3; ++id) f.run(f.limit(id, Side::Sell, 100, 5));
    auto events = f.run(f.cancel(position + 1, Side::Sell));
    CHECK(events.size() == 1); CHECK(events[0].type == EventType::Cancelled); CHECK(events[0].quantity == 5);
    auto trades = f.trades(f.run(f.market(4, Side::Buy, 10)));
    CHECK(trades.size() == 2);
    std::vector<OrderId> expected; for (OrderId id = 1; id <= 3; ++id) if (id != position + 1) expected.push_back(id);
    CHECK(trades[0].contra_id == expected[0]); CHECK(trades[1].contra_id == expected[1]); CHECK(f.book.size() == 0);
}
template<class B> void cancel_front() { cancel_position<B>(0); }
template<class B> void cancel_middle() { cancel_position<B>(1); }
template<class B> void cancel_tail() { cancel_position<B>(2); }
template<class B> void cancel_only_removes_level() {
    Fixture<B> f; f.run(f.limit(1, Side::Buy, 100, 8)); f.run(f.cancel(1, Side::Buy));
    CHECK(f.book.snapshot().levels.empty()); CHECK(f.book.resting_quantity() == 0);
}
template<class B> void replace_reduction_loses_priority() {
    Fixture<B> f; f.run(f.limit(1, Side::Sell, 100, 10)); f.run(f.limit(2, Side::Sell, 100, 10));
    auto events = f.run(f.replace(1, Side::Sell, 100, 5));
    CHECK(events.size() == 2); CHECK(events[0].type == EventType::Cancelled); CHECK(events[1].type == EventType::Accepted);
    auto trades = f.trades(f.run(f.market(3, Side::Buy, 11)));
    CHECK(trades[0].contra_id == 2); CHECK(trades[1].contra_id == 1);
}
template<class B> void invalid_replace_preserves_original() {
    Fixture<B> f; f.run(f.limit(1, Side::Sell, 100, 10)); f.run(f.limit(2, Side::Sell, 100, 10));
    const auto before = f.book.snapshot();
    f.reject(f.replace(1, Side::Sell, 201, 5), RejectReason::InvalidPrice);
    CHECK(f.book.snapshot().orders == before.orders); CHECK(f.book.snapshot().levels == before.levels);
    f.reject(f.replace(1, Side::Sell, 100, 0), RejectReason::InvalidQuantity);
    CHECK(f.book.snapshot().orders == before.orders);
    f.reject(f.replace(1, Side::Buy, 100, 5), RejectReason::WrongSide);
    CHECK(f.book.snapshot().orders == before.orders);
}
template<class B> void replace_fok_rejection_atomic() {
    Fixture<B> f; f.run(f.limit(1, Side::Buy, 90, 10)); f.run(f.limit(2, Side::Sell, 100, 4));
    const auto before = f.book.snapshot();
    f.reject(f.replace(1, Side::Buy, 100, 5, TimeInForce::FOK), RejectReason::WouldNotFill);
    CHECK(f.book.snapshot().orders == before.orders); CHECK(f.book.snapshot().levels == before.levels);
    CHECK(f.book.stats().executed == before.stats.executed); CHECK(f.book.stats().cancelled == before.stats.cancelled);
}
template<class B> void replace_can_cross() {
    Fixture<B> f; f.run(f.limit(1, Side::Buy, 90, 5)); f.run(f.limit(2, Side::Sell, 100, 5));
    auto events = f.run(f.replace(1, Side::Buy, 100, 5));
    CHECK(events.size() == 5); CHECK(events[0].type == EventType::Cancelled); CHECK(events[1].type == EventType::Accepted);
    CHECK(events[2].type == EventType::Trade); CHECK(events[2].price == 100); CHECK(f.book.size() == 0);
}
template<class B> void ioc_partial_expires_residual() {
    Fixture<B> f; f.run(f.limit(1, Side::Sell, 100, 3));
    auto events = f.run(f.limit(2, Side::Buy, 100, 5, TimeInForce::IOC));
    CHECK(events.back().type == EventType::Expired); CHECK(events.back().quantity == 2);
    CHECK(f.book.size() == 0); CHECK(f.book.stats().expired == 2);
}
template<class B> void ioc_non_crossing_never_rests() {
    Fixture<B> f; f.run(f.limit(1, Side::Sell, 110, 3));
    auto events = f.run(f.limit(2, Side::Buy, 100, 5, TimeInForce::IOC));
    CHECK(events.size() == 2); CHECK(events.back().type == EventType::Expired); CHECK(f.book.size() == 1);
}
template<class B> void fok_multiple_levels_atomic() {
    Fixture<B> f; f.run(f.limit(1, Side::Sell, 100, 3)); f.run(f.limit(2, Side::Sell, 105, 4)); f.run(f.limit(3, Side::Sell, 110, 9));
    const auto before = f.book.snapshot();
    f.reject(f.limit(4, Side::Buy, 105, 8, TimeInForce::FOK), RejectReason::WouldNotFill);
    CHECK(f.book.snapshot().orders == before.orders); CHECK(f.book.stats().trade_count == 0);
    auto trades = f.trades(f.run(f.limit(4, Side::Buy, 105, 7, TimeInForce::FOK)));
    CHECK(trades.size() == 2); CHECK(trades[0].price == 100); CHECK(trades[1].price == 105); CHECK(f.book.size() == 1);
}
template<class B> void market_empty_expires() {
    Fixture<B> f; auto events = f.run(f.market(1, Side::Buy, 9));
    CHECK(events.size() == 2); CHECK(events[0].type == EventType::Accepted); CHECK(events[1].type == EventType::Expired);
    CHECK(events[1].quantity == 9); CHECK(f.book.size() == 0);
}
template<class B> void market_ignores_price_and_never_rests() {
    Fixture<B> f; f.run(f.limit(1, Side::Buy, 90, 3));
    auto command = f.market(2, Side::Sell, 5); command.price = std::numeric_limits<Price>::min();
    auto events = f.run(command); CHECK(events.back().type == EventType::Expired); CHECK(events.back().quantity == 2); CHECK(f.book.size() == 0);
}
template<class B> void market_fok_requires_all_liquidity() {
    Fixture<B> f; f.run(f.limit(1, Side::Sell, 200, 3));
    f.reject(f.market(2, Side::Buy, 4, TimeInForce::FOK), RejectReason::WouldNotFill);
    CHECK(f.book.resting_quantity() == 3); f.run(f.market(2, Side::Buy, 3, TimeInForce::FOK)); CHECK(f.book.size() == 0);
}
template<class B> void full_capacity_conservative_gtc_rejection() {
    Fixture<B> f; for (OrderId id = 1; id <= 16; ++id) f.run(f.limit(id, Side::Sell, 100, 1));
    const auto before = f.book.snapshot(); f.reject(f.limit(17, Side::Buy, 100, 16), RejectReason::Capacity);
    CHECK(f.book.snapshot().orders == before.orders); CHECK(f.book.stats().executed == 0);
}
template<class B> void full_capacity_ioc_fok_market_allowed() {
    for (int kind = 0; kind < 3; ++kind) {
        Fixture<B> f; for (OrderId id = 1; id <= 16; ++id) f.run(f.limit(id, Side::Sell, 100, 1));
        auto command = kind == 2 ? f.market(17, Side::Buy, 16) : f.limit(17, Side::Buy, 100, 16, kind ? TimeInForce::FOK : TimeInForce::IOC);
        f.run(command); CHECK(f.book.size() == 0);
    }
}
template<class B> void full_capacity_replace_reuses_slot() {
    Fixture<B> f; for (OrderId id = 1; id <= 16; ++id) f.run(f.limit(id, Side::Sell, 100, 1));
    f.run(f.replace(1, Side::Sell, 101, 2)); CHECK(f.book.size() == 16); CHECK(f.book.resting_quantity() == 17);
}
template<class B> void maximum_quantity_and_wide_level_sum() {
    Fixture<B> f; constexpr auto max = std::numeric_limits<Quantity>::max();
    f.run(f.limit(1, Side::Sell, 100, max)); f.run(f.limit(2, Side::Sell, 100, max));
    CHECK(f.book.resting_quantity() == 2ULL * max); CHECK(f.book.snapshot().levels[0].quantity == 2ULL * max);
    f.run(f.market(3, Side::Buy, max)); CHECK(f.book.resting_quantity() == max);
    f.run(f.cancel(2, Side::Sell)); CHECK(f.book.size() == 0);
}
template<class B> void maximum_id_and_id_reuse() {
    Fixture<B> f; constexpr auto max = std::numeric_limits<OrderId>::max();
    f.run(f.limit(max, Side::Sell, 100, 1)); f.run(f.market(1, Side::Buy, 1));
    f.run(f.limit(max, Side::Buy, 90, 2)); f.run(f.cancel(max, Side::Buy)); f.run(f.limit(max, Side::Sell, 110, 3));
    CHECK(f.book.snapshot().orders[0].id == max);
}
template<class B> void duplicate_id_rejected_for_all_tif() {
    Fixture<B> f; f.run(f.limit(1, Side::Sell, 100, 7));
    for (auto tif : {TimeInForce::GTC, TimeInForce::IOC, TimeInForce::FOK}) f.reject(f.limit(1, Side::Buy, 100, 1, tif), RejectReason::DuplicateId);
    f.reject(f.market(1, Side::Buy, 1), RejectReason::DuplicateId); CHECK(f.book.resting_quantity() == 7);
}
template<class B> void unknown_id_and_wrong_side() {
    Fixture<B> f; f.reject(f.cancel(55, Side::Buy), RejectReason::UnknownId); f.reject(f.replace(55, Side::Buy, 100, 5), RejectReason::UnknownId);
    f.run(f.limit(1, Side::Buy, 90, 5)); f.reject(f.replace(1, Side::Sell, 100, 5), RejectReason::WrongSide); CHECK(f.book.resting_quantity() == 5);
    auto events = f.run(f.cancel(1, Side::Sell)); CHECK(events.front().side == Side::Buy); CHECK(f.book.size() == 0);
}
template<class B> void invalid_id_price_quantity() {
    Fixture<B> f; f.reject(f.limit(0, Side::Buy, 100, 5), RejectReason::InvalidId);
    f.reject(f.limit(1, Side::Buy, 0, 5), RejectReason::InvalidPrice); f.reject(f.limit(1, Side::Buy, 201, 5), RejectReason::InvalidPrice);
    f.reject(f.limit(1, Side::Buy, 100, 0), RejectReason::InvalidQuantity); f.reject(f.market(1, Side::Buy, 0), RejectReason::InvalidQuantity);
    CHECK(f.book.size() == 0);
}
template<class B> void invalid_enum_values() {
    Fixture<B> f;
    auto command = f.limit(1, Side::Buy, 100, 5); command.side = static_cast<Side>(255); f.reject(command, RejectReason::InvalidCommand);
    command = f.limit(1, Side::Buy, 100, 5); command.action = static_cast<Action>(255); f.reject(command, RejectReason::InvalidCommand);
    command = f.limit(1, Side::Buy, 100, 5); command.tif = static_cast<TimeInForce>(255); f.reject(command, RejectReason::InvalidCommand);
}
template<class B> void undersized_output_is_atomic_and_cleared() {
    Fixture<B> f; f.run(f.limit(1, Side::Sell, 100, 7)); const auto before = f.book.snapshot();
    std::array<Event, required_event_capacity(16) - 1> storage{}; EventBuffer output{storage}; CHECK(output.push(Event{}));
    CHECK(f.book.process(f.market(2, Side::Buy, 7), output) == ProcessResult::OutputFull); CHECK(output.events().empty());
    CHECK(f.book.snapshot() == before); CHECK(f.book.check_invariants());
    std::span<Event> empty; EventBuffer empty_output{empty}; CHECK(f.book.process(f.cancel(1, Side::Sell), empty_output) == ProcessResult::OutputFull);
    CHECK(f.book.snapshot() == before);
}
template<class B> void output_buffer_reused_clears_previous_reports() {
    Fixture<B> f; f.run(f.limit(1, Side::Sell, 100, 5)); f.run(f.market(2, Side::Buy, 5));
    auto events = f.run(f.limit(3, Side::Buy, 90, 2)); CHECK(events.size() == 1); CHECK(events[0].id == 3); CHECK(events[0].type == EventType::Accepted);
}
template<class B> void snapshot_order_and_boundary_prices() {
    Fixture<B> f; f.run(f.limit(1, Side::Buy, 1, 2)); f.run(f.limit(2, Side::Buy, 50, 2));
    f.run(f.limit(3, Side::Sell, 200, 2)); f.run(f.limit(4, Side::Sell, 150, 2)); f.run(f.limit(5, Side::Sell, 150, 3));
    CHECK(f.book.snapshot().orders == std::vector<RestingOrder>{{2, Side::Buy, 50, 2}, {1, Side::Buy, 1, 2}, {4, Side::Sell, 150, 2}, {5, Side::Sell, 150, 3}, {3, Side::Sell, 200, 2}});
}
template<class B> void sparse_bitmap_word_boundaries() {
    Fixture<B> f; f.run(f.limit(1, Side::Sell, 64, 1)); f.run(f.limit(2, Side::Sell, 65, 1)); f.run(f.limit(3, Side::Sell, 129, 1));
    auto trades = f.trades(f.run(f.market(4, Side::Buy, 3))); CHECK(trades.size() == 3);
    CHECK(trades[0].price == 64); CHECK(trades[1].price == 65); CHECK(trades[2].price == 129); CHECK(f.book.snapshot().levels.empty());
}
template<class B> void cancel_replace_after_partial_fill() {
    Fixture<B> f; f.run(f.limit(1, Side::Sell, 100, 10)); f.run(f.market(2, Side::Buy, 3)); f.run(f.replace(1, Side::Sell, 110, 5));
    CHECK(f.book.stats().cancelled == 7); f.run(f.cancel(1, Side::Sell)); CHECK(f.book.stats().cancelled == 12); CHECK(f.book.size() == 0);
}
template<class B> void malformed_replace_enum_preserves_original() {
    Fixture<B> f; f.run(f.limit(1, Side::Buy, 90, 10)); const auto before = f.book.snapshot();
    auto command = f.replace(1, Side::Buy, 100, 3); command.tif = static_cast<TimeInForce>(3);
    f.reject(command, RejectReason::InvalidCommand); CHECK(f.book.snapshot().orders == before.orders); CHECK(f.book.stats().cancelled == 0);
}
template<class B> void rejected_command_counts_outside_conservation() {
    Fixture<B> f; f.run(f.limit(1, Side::Buy, 90, 3)); f.reject(f.limit(1, Side::Buy, 90, 7), RejectReason::DuplicateId);
    CHECK(f.book.stats().submitted == 3); CHECK(f.book.stats().rejected == 7); CHECK(f.book.stats().executed == 0);
}
template<class B> void invalid_configuration_rejected() {
    for (const auto config : {Config{0, 100, 16}, Config{-1, std::numeric_limits<Price>::max(), 16}, Config{100, 99, 16}, Config{1, 100, 0}}) {
        bool rejected = false; try { B book{config}; } catch (const std::exception&) { rejected = true; } CHECK(rejected);
    }
}

using Case = std::pair<std::string, std::function<void()>>;
template<class B> void add_cases(std::vector<Case>& cases, const std::string& prefix) {
#define ADD(name) cases.emplace_back(prefix + #name, &name<B>)
    ADD(fifo_same_price); ADD(best_price_before_time); ADD(sell_taker_best_bid); ADD(partial_fill_resting_residual); ADD(partial_maker_event_order);
    ADD(cancel_front); ADD(cancel_middle); ADD(cancel_tail); ADD(cancel_only_removes_level); ADD(replace_reduction_loses_priority);
    ADD(invalid_replace_preserves_original); ADD(replace_fok_rejection_atomic); ADD(replace_can_cross); ADD(ioc_partial_expires_residual);
    ADD(ioc_non_crossing_never_rests); ADD(fok_multiple_levels_atomic); ADD(market_empty_expires); ADD(market_ignores_price_and_never_rests);
    ADD(market_fok_requires_all_liquidity); ADD(full_capacity_conservative_gtc_rejection); ADD(full_capacity_ioc_fok_market_allowed);
    ADD(full_capacity_replace_reuses_slot); ADD(maximum_quantity_and_wide_level_sum); ADD(maximum_id_and_id_reuse); ADD(duplicate_id_rejected_for_all_tif);
    ADD(unknown_id_and_wrong_side); ADD(invalid_id_price_quantity); ADD(invalid_enum_values); ADD(undersized_output_is_atomic_and_cleared);
    ADD(output_buffer_reused_clears_previous_reports); ADD(snapshot_order_and_boundary_prices); ADD(sparse_bitmap_word_boundaries);
    ADD(cancel_replace_after_partial_fill); ADD(malformed_replace_enum_preserves_original); ADD(rejected_command_counts_outside_conservation); ADD(invalid_configuration_rejected);
#undef ADD
}
#ifdef LEDGER_USE_GTEST
class FunctionalTest : public ::testing::Test {
public: explicit FunctionalTest(std::function<void()> function) : function_(std::move(function)) {}
private: void TestBody() override { function_(); } std::function<void()> function_;
};
#endif
}

int main(int argc, char** argv) {
    std::vector<Case> cases; add_cases<ledger::FastBook>(cases, "FastBook."); add_cases<ledger::ReferenceBook>(cases, "ReferenceBook.");
#ifdef LEDGER_USE_GTEST
    ::testing::InitGoogleTest(&argc, argv);
    for (auto& [name, function] : cases) {
        const auto separator = name.find('.');
        ::testing::RegisterTest(name.substr(0, separator).c_str(), name.substr(separator + 1).c_str(), nullptr, nullptr, __FILE__, __LINE__, [function] { return new FunctionalTest(function); });
    }
    return RUN_ALL_TESTS();
#else
    (void)argc; (void)argv;
    unsigned failed = 0;
    for (const auto& [name, function] : cases) {
        try { function(); std::cout << "PASS " << name << '\n'; }
        catch (const std::exception& error) { ++failed; std::cerr << "FAIL " << name << ": " << error.what() << '\n'; }
    }
    std::cout << cases.size() - failed << '/' << cases.size() << " named semantic tests passed\n";
    return failed ? 1 : 0;
#endif
}
