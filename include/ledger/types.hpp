#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace ledger {
using OrderId = std::uint64_t;
using Price = std::int64_t;
using Quantity = std::uint32_t;
enum class Side : std::uint8_t { Buy = 0, Sell = 1 };
enum class Action : std::uint8_t { Limit = 0, Market = 1, Cancel = 2, Replace = 3 };
enum class TimeInForce : std::uint8_t { GTC = 0, IOC = 1, FOK = 2 };
struct Command {
    std::uint64_t sequence{};
    OrderId id{};
    Price price{};
    Quantity quantity{};
    Side side{Side::Buy};
    Action action{Action::Limit};
    TimeInForce tif{TimeInForce::GTC};
    bool operator==(const Command&) const = default;
};
enum class EventType : std::uint8_t { Accepted, Trade, PartiallyFilled, Filled, Cancelled, Expired, Rejected };
enum class RejectReason : std::uint8_t { None, InvalidCommand, InvalidId, InvalidPrice, InvalidQuantity, DuplicateId, UnknownId, Capacity, WouldNotFill, WrongSide };
struct Event {
    std::uint64_t sequence{};
    EventType type{};
    RejectReason reason{RejectReason::None};
    OrderId id{};
    OrderId contra_id{};
    Side side{Side::Buy};
    Price price{};
    Quantity quantity{};
    Quantity remaining{};
    bool operator==(const Event&) const = default;
};
// Fixed storage owned by the caller; no callback and no hot-path growth.
class EventBuffer {
public:
    explicit EventBuffer(std::span<Event> storage) noexcept : storage_(storage) {}
    void clear() noexcept { size_ = 0; }
    bool push(const Event& event) noexcept {
        if (size_ == storage_.size()) return false;
        storage_[size_++] = event; return true;
    }
    std::size_t capacity() const noexcept { return storage_.size(); }
    std::span<const Event> events() const noexcept { return storage_.first(size_); }
private:
    std::span<Event> storage_;
    std::size_t size_{};
};
enum class ProcessResult { Applied, Rejected, OutputFull };
struct Config {
    Price min_price{1};
    Price max_price{20000};
    std::uint32_t max_orders{8192};
};
constexpr std::size_t required_event_capacity(std::size_t max_orders) noexcept { return 3 * max_orders + 4; }
struct RestingOrder {
    OrderId id{}; Side side{}; Price price{}; Quantity quantity{};
    bool operator==(const RestingOrder&) const = default;
};
struct LevelSnapshot {
    Side side{}; Price price{}; std::uint64_t quantity{}; std::uint32_t orders{};
    bool operator==(const LevelSnapshot&) const = default;
};
struct Statistics {
    std::uint64_t submitted{}, executed{}, cancelled{}, expired{}, rejected{}, trade_count{};
    bool operator==(const Statistics&) const = default;
};
// snapshot() is diagnostic and may allocate. process() is the measured hot path.
struct Snapshot {
    std::vector<RestingOrder> orders; // Buy descending, then Sell ascending; FIFO within a level.
    std::vector<LevelSnapshot> levels;
    Statistics stats;
    bool operator==(const Snapshot&) const = default;
};
}
