#include "ledger/book.hpp"
#include "ledger/reference_book.hpp"
#include "ledger/study_observer.hpp"

#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
using namespace ledger;
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }

template<class Book> struct Fixture {
    Book book{Config{1, 256, 32}};
    study::EventObserver observer{book.snapshot()};
    std::vector<Event> storage = std::vector<Event>(required_event_capacity(32));
    EventBuffer output{storage};
    std::uint64_t sequence{};
    study::Mechanics run(OrderId id, Side side, Action action, Price price, Quantity quantity,
                         TimeInForce tif = TimeInForce::GTC) {
        const Command command{++sequence, id, price, quantity, side, action, tif};
        const auto result = book.process(command, output);
        const auto mechanics = observer.apply(command, result, output.events());
        check(observer.matches(book.snapshot()), "observer must match FIFO orders and aggregate levels after every command");
        return mechanics;
    }
};
template<class Book> void semantic_cases() {
    Fixture<Book> fixture;
    fixture.run(1, Side::Sell, Action::Limit, 100, 3);
    fixture.run(2, Side::Sell, Action::Limit, 100, 5);
    fixture.run(3, Side::Sell, Action::Limit, 101, 4);
    const auto partial = fixture.run(4, Side::Buy, Action::Market, 0, 2);
    check(partial.fills == 1 && partial.filled_quantity == 2 && partial.levels_emptied == 0,
          "partial maker execution must keep its level");
    const auto sweep = fixture.run(5, Side::Buy, Action::Market, 0, 8);
    check(sweep.fills == 3 && sweep.filled_quantity == 8 && sweep.levels_emptied == 1,
          "multi-maker sweep must count trade reports and only actually emptied levels");
    const auto cancelled = fixture.run(3, Side::Buy, Action::Cancel, 0, 0);
    check(cancelled.fills == 0 && cancelled.levels_emptied == 1, "cancel side is ignored and removes last level");
    fixture.run(6, Side::Buy, Action::Limit, 90, 4);
    fixture.run(7, Side::Buy, Action::Limit, 90, 5);
    const auto replace_middle = fixture.run(6, Side::Buy, Action::Replace, 90, 2);
    check(replace_middle.levels_emptied == 0, "same level replace must preserve other resting order");
    const auto rejected = fixture.run(6, Side::Buy, Action::Replace, 90, 100, TimeInForce::FOK);
    check(rejected.fills == 0 && rejected.levels_emptied == 0, "failed FOK replacement must preserve prior order atomically");
    const auto fifo = fixture.run(8, Side::Sell, Action::Market, 0, 6);
    check(fifo.fills == 2 && fifo.levels_emptied == 0, "replacement moves FIFO priority and can leave residual");
    const auto recreate = fixture.run(6, Side::Buy, Action::Replace, 90, 3);
    check(recreate.levels_emptied == 1, "replace records removal even if it recreates the same level");
    fixture.run(9, Side::Sell, Action::Limit, 110, 1);
    const auto expired = fixture.run(10, Side::Buy, Action::Market, 0, 2);
    check(expired.fills == 1 && expired.filled_quantity == 1 && expired.levels_emptied == 1,
          "market residual expiration does not add a resting order");
    fixture.run(11, Side::Sell, Action::Limit, 100, 1);
    const auto residual = fixture.run(12, Side::Buy, Action::Limit, 100, 3);
    check(residual.fills == 1 && residual.levels_emptied == 1, "crossing GTC must reconstruct resting residual");
}
template<class Book> void wide_quantity_case() {
    Fixture<Book> fixture;
    const auto max = std::numeric_limits<Quantity>::max();
    fixture.run(1, Side::Sell, Action::Limit, 100, max);
    fixture.run(2, Side::Sell, Action::Limit, 100, max);
    check(fixture.book.resting_quantity() == 2ULL * max, "wide aggregated quantity");
    const auto trade = fixture.run(3, Side::Buy, Action::Market, 0, max);
    check(trade.filled_quantity == max && trade.levels_emptied == 0, "wide maker quantity exact accounting");
    const auto remove = fixture.run(2, Side::Sell, Action::Cancel, 0, 0);
    check(remove.levels_emptied == 1, "wide level cancellation");
}
void initial_snapshot_and_tamper() {
    ReferenceBook book{Config{1, 256, 32}};
    std::vector<Event> storage(required_event_capacity(32));
    EventBuffer output{storage};
    const Command initial{1, 1, 100, 3, Side::Sell, Action::Limit, TimeInForce::GTC};
    check(book.process(initial, output) == ProcessResult::Applied, "initial resting order");
    study::EventObserver observer(book.snapshot());
    check(observer.matches(book.snapshot()), "observer initial snapshot must retain FIFO order");
    const Command trade{2, 2, 0, 2, Side::Buy, Action::Market, TimeInForce::GTC};
    const auto status = book.process(trade, output);
    std::vector<Event> tampered(output.events().begin(), output.events().end());
    for (auto& event : tampered) if (event.type == EventType::Trade) event.quantity = 4;
    bool rejected = false;
    try { observer.apply(trade, status, tampered); } catch (const std::runtime_error&) { rejected = true; }
    check(rejected, "observer must reject report quantity underflow");
}
} // namespace

int main() {
    try {
        semantic_cases<ReferenceBook>();
        semantic_cases<FastBook>();
        wide_quantity_case<ReferenceBook>();
        wide_quantity_case<FastBook>();
        initial_snapshot_and_tamper();
        std::cout << "Study observer: FIFO, partial/multi-maker fills, cancellation, replace, atomic FOK, residuals, wide quantities and tamper checks pass.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Study observer test failed: " << error.what() << '\n';
        return 1;
    }
}
