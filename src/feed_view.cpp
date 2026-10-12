#include "ledger/feed_view.hpp"

namespace ledger {

bool FeedView::parse(std::span<const std::byte> bytes, FeedView& out,
                     std::uint64_t max_records) noexcept {
    if (bytes.size() < feed_header_size) return false;
    FeedHeader header{};
    if (!decode_header(bytes.first(feed_header_size), header, max_records)) return false;

    const auto payload = bytes.subspan(feed_header_size);
    // Division and remainder avoid record_count-controlled multiplication or
    // narrowing. Hostile counts cannot trigger an oversized subspan or scan.
    if (payload.size() % feed_record_size != 0
        || header.record_count != payload.size() / feed_record_size) return false;

    auto remaining = payload;
    while (!remaining.empty()) {
        Command command{};
        if (!decode_record(remaining.first(feed_record_size), command)) return false;
        remaining = remaining.subspan(feed_record_size);
    }

    // Publication happens only after every record, including the last, is valid.
    out = FeedView(payload);
    return true;
}

bool FeedView::read(std::size_t index, Command& out) const noexcept {
    if (!valid_ || index >= size()) return false;
    // index < payload.size() / record_size proves both multiplication and the
    // entire record range are representable and within the borrowed span.
    return decode_record(records_.subspan(index * feed_record_size, feed_record_size), out);
}

}
