#include "ledger/study_workloads.hpp"

#include "ledger/book.hpp"
#include "ledger/reference_book.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace ledger;
std::uint64_t assertions{};
void require(bool condition, const char* expression, int line) {
    ++assertions;
    if (!condition) throw std::runtime_error(std::string{"line "} + std::to_string(line) + ": " + expression);
}
#define CHECK(...) require((__VA_ARGS__), #__VA_ARGS__, __LINE__)

struct Counts {
    std::array<std::uint64_t, 4> actions{};
    std::uint64_t applied{}, rejected{}, successful_cancels{}, successful_replaces{},
        successful_fok{}, rejected_fok{}, trades{}, peak_live{};
    std::set<std::size_t> fill_counts;
    std::set<RejectReason> reasons;
};

Counts replay(const study::Workload& workload, bool fast_compare = true) {
    ReferenceBook reference{workload.config};
    std::unique_ptr<FastBook> bitmap, linear;
    if (fast_compare) {
        bitmap = std::make_unique<FastBook>(workload.config, true);
        linear = std::make_unique<FastBook>(workload.config, false);
    }
    std::vector<Event> reference_storage(required_event_capacity(workload.config.max_orders));
    std::vector<Event> bitmap_storage(reference_storage.size()), linear_storage(reference_storage.size());
    EventBuffer reference_output{reference_storage}, bitmap_output{bitmap_storage}, linear_output{linear_storage};
    Counts counts;
    std::uint64_t sequence = 0;
    const auto process = [&](const Command& command, bool preload) {
        CHECK(command.sequence == ++sequence);
        const auto before_quantity = reference.resting_quantity();
        const auto before_size = reference.size();
        const auto result = reference.process(command, reference_output);
        CHECK(result != ProcessResult::OutputFull);
        if (fast_compare) {
            CHECK(bitmap->process(command, bitmap_output) == result);
            CHECK(linear->process(command, linear_output) == result);
            CHECK(std::ranges::equal(reference_output.events(), bitmap_output.events()));
            CHECK(std::ranges::equal(reference_output.events(), linear_output.events()));
        }
        CHECK(reference.size() <= workload.config.max_orders);
        counts.peak_live = std::max<std::uint64_t>(counts.peak_live, reference.size());
        if (preload) {
            CHECK(result == ProcessResult::Applied);
            CHECK(reference_output.events().size() == 1);
            CHECK(reference_output.events()[0].type == EventType::Accepted);
            return;
        }
        const auto action = static_cast<std::size_t>(command.action);
        CHECK(action < counts.actions.size());
        ++counts.actions[action];
        if (result == ProcessResult::Rejected) {
            ++counts.rejected;
            CHECK(reference.size() == before_size);
            CHECK(reference.resting_quantity() == before_quantity);
            CHECK(reference_output.events().size() == 1);
            counts.reasons.insert(reference_output.events()[0].reason);
            if (command.tif == TimeInForce::FOK) {
                ++counts.rejected_fok;
                // FOK failures are not shortcuts caused by an empty contra side.
                CHECK(command.side == Side::Buy ? reference.best_ask() != 0 : reference.best_bid() != 0);
                CHECK(command.action == Action::Limit);
                CHECK(command.side == Side::Buy ? command.price == workload.config.max_price
                                               : command.price == workload.config.min_price);
            }
        } else {
            ++counts.applied;
            if (command.action == Action::Cancel) ++counts.successful_cancels;
            if (command.action == Action::Replace) ++counts.successful_replaces;
            if (command.tif == TimeInForce::FOK) ++counts.successful_fok;
        }
        std::size_t fills = 0;
        for (const auto& event : reference_output.events()) if (event.type == EventType::Trade) ++fills;
        counts.trades += fills;
        if (fills) counts.fill_counts.insert(fills);
        if (sequence % 512 == 0) {
            CHECK(reference.check_invariants());
            if (fast_compare) {
                CHECK(bitmap->check_invariants());
                CHECK(linear->check_invariants());
                CHECK(bitmap->snapshot() == reference.snapshot());
                CHECK(linear->snapshot() == reference.snapshot());
            }
        }
    };
    for (const auto& command : workload.preload) process(command, true);
    CHECK(reference.size() == workload.preload.size());
    CHECK(reference.best_bid() < reference.best_ask());
    for (const auto& command : workload.commands) process(command, false);
    CHECK(reference.check_invariants());
    if (fast_compare) {
        CHECK(bitmap->check_invariants());
        CHECK(linear->check_invariants());
        CHECK(bitmap->snapshot() == reference.snapshot());
        CHECK(linear->snapshot() == reference.snapshot());
    }
    CHECK(counts.applied + counts.rejected == workload.commands.size());
    CHECK(reference.stats().trade_count == counts.trades);
    return counts;
}

void profiles_and_parameter_bounds() {
    CHECK(study::profiles() == std::vector<std::string>{"balanced", "cancel_heavy", "trade_storm",
                                                     "replace_heavy", "sparse_fok", "rejection_stress"});
    const auto rejects = [](std::string_view profile, std::size_t count, std::size_t depth, Price domain) {
        bool threw = false;
        try { (void)study::make_workload(profile, count, 1, depth, domain); }
        catch (const std::invalid_argument&) { threw = true; }
        CHECK(threw);
    };
    rejects("absent", 1, 256, 20000);
    rejects("balanced", 1000001, 256, 20000);
    rejects("balanced", 1, 0, 20000);
    rejects("balanced", 1, 16385, 65536);
    rejects("balanced", 1, std::numeric_limits<std::size_t>::max(), 20000);
    rejects("balanced", 1, 64, 127);
    rejects("balanced", 1, 1, -1);
    rejects("balanced", 1, 1, std::numeric_limits<Price>::min());
    rejects("balanced", 1, 1, 1000001);
    rejects("balanced", 1, 1, std::numeric_limits<Price>::max());
}

void deterministic_but_seed_sensitive() {
    for (const auto& profile : study::profiles()) {
        const auto first = study::make_workload(profile, 2000, 17, 64, 4096);
        const auto same = study::make_workload(profile, 2000, 17, 64, 4096);
        const auto different = study::make_workload(profile, 2000, 18, 64, 4096);
        CHECK(first.preload == same.preload);
        CHECK(first.commands == same.commands);
        CHECK(first.preload != different.preload);
        CHECK(first.commands != different.commands);
        CHECK(first.commands.size() == 2000);
        CHECK(first.preload.size() == 128);
        CHECK(first.config.min_price == 1 && first.config.max_price == 4096);
        CHECK(first.config.max_orders == 1024);
    }
}

void genuine_initial_depth_and_dispersion() {
    for (const auto depth : {std::size_t{1}, std::size_t{64}, std::size_t{1024}, std::size_t{16384}}) {
        const Price domain = static_cast<Price>(std::max<std::size_t>(2 * depth, 65536));
        const auto workload = study::make_workload("balanced", 0, 5, depth, domain);
        CHECK(workload.preload.size() == 2 * depth);
        CHECK(workload.commands.empty());
        CHECK(workload.config.max_orders == std::max<std::size_t>(4 * depth, 1024));
        std::array<std::set<Price>, 2> prices;
        for (const auto& command : workload.preload) {
            CHECK(command.action == Action::Limit && command.tif == TimeInForce::GTC);
            CHECK(command.price >= 1 && command.price <= domain);
            CHECK(command.quantity > 0);
            prices[command.side == Side::Buy ? 0 : 1].insert(command.price);
        }
        CHECK(prices[0].size() == depth && prices[1].size() == depth);
        CHECK(*prices[0].rbegin() < *prices[1].begin());
        if (depth > 1) {
            CHECK(*prices[0].begin() == 1);
            CHECK(*prices[1].rbegin() == domain);
        }
        // No fast allocation needed for this maximum-depth preload-only check.
        (void)replay(workload, false);
    }
    const auto narrow = study::make_workload("balanced", 0, 5, 64, 128);
    const auto wide = study::make_workload("balanced", 0, 5, 64, 1000000);
    CHECK(narrow.preload != wide.preload);
    CHECK(wide.preload.front().price == 500000);
}

void six_profiles_differential_replay() {
    for (const auto& profile : study::profiles()) {
        const auto workload = study::make_workload(profile, 6000, 1729, 64, 4096);
        const auto counts = replay(workload);
        if (profile == "rejection_stress") CHECK(counts.rejected > 4000);
        else if (profile != "sparse_fok") CHECK(counts.rejected == 0);
    }
}

void bounded_smallest_domain_and_depth() {
    for (const auto& profile : study::profiles()) {
        const auto workload = study::make_workload(profile, 600, 0, 1, 2);
        const auto counts = replay(workload);
        CHECK(counts.peak_live <= workload.config.max_orders);
    }
}

void profile_mechanisms_and_realized_mix() {
    constexpr std::size_t count = 20000;
    constexpr std::uint64_t seed = 8675309;
    const auto balanced = replay(study::make_workload("balanced", count, seed, 256, 20000), false);
    CHECK(balanced.rejected == 0);
    for (const auto actions : balanced.actions) CHECK(actions > 0);
    CHECK(balanced.trades > 0);

    const auto cancels = replay(study::make_workload("cancel_heavy", count, seed, 256, 20000), false);
    CHECK(cancels.rejected == 0);
    CHECK(cancels.actions[static_cast<std::size_t>(Action::Cancel)] == cancels.successful_cancels);
    CHECK(cancels.successful_cancels > 9500 && cancels.successful_cancels < 10500);
    CHECK(cancels.actions[static_cast<std::size_t>(Action::Market)] == 0);
    CHECK(cancels.actions[static_cast<std::size_t>(Action::Replace)] == 0);

    const auto storm = replay(study::make_workload("trade_storm", count, seed, 256, 20000), false);
    CHECK(storm.rejected == 0);
    CHECK(storm.fill_counts.size() == 64);
    CHECK(*storm.fill_counts.begin() == 1 && *storm.fill_counts.rbegin() == 64);
    CHECK(storm.trades > 15000);

    const auto replacements = replay(study::make_workload("replace_heavy", count, seed, 256, 20000), false);
    CHECK(replacements.rejected == 0);
    CHECK(replacements.actions[static_cast<std::size_t>(Action::Replace)] == replacements.successful_replaces);
    CHECK(replacements.successful_replaces > 5500 && replacements.successful_replaces < 6500);

    const auto sparse = replay(study::make_workload("sparse_fok", count, seed, 256, 20000), false);
    CHECK(sparse.successful_fok > 300 && sparse.rejected_fok > 300);
    CHECK(sparse.rejected == sparse.rejected_fok);
    CHECK(sparse.reasons == std::set<RejectReason>{RejectReason::WouldNotFill});
    CHECK(sparse.fill_counts.size() > 50);

    const auto rejections = replay(study::make_workload("rejection_stress", count, seed, 256, 20000), false);
    CHECK(rejections.rejected > 15500 && rejections.rejected < 16500);
    CHECK(rejections.reasons == std::set<RejectReason>{RejectReason::UnknownId, RejectReason::DuplicateId,
        RejectReason::InvalidPrice, RejectReason::InvalidQuantity, RejectReason::InvalidId,
        RejectReason::WrongSide, RejectReason::InvalidCommand, RejectReason::WouldNotFill});
    CHECK(rejections.successful_replaces == rejections.applied);
}

} // namespace

int main() {
    const std::array<std::pair<const char*, std::function<void()>>, 6> cases{{
        {"profiles_and_parameter_bounds", profiles_and_parameter_bounds},
        {"deterministic_but_seed_sensitive", deterministic_but_seed_sensitive},
        {"genuine_initial_depth_and_dispersion", genuine_initial_depth_and_dispersion},
        {"six_profiles_differential_replay", six_profiles_differential_replay},
        {"bounded_smallest_domain_and_depth", bounded_smallest_domain_and_depth},
        {"profile_mechanisms_and_realized_mix", profile_mechanisms_and_realized_mix}}};
    unsigned failed = 0;
    for (const auto& [name, test] : cases) {
        try { test(); std::cout << "PASS " << name << '\n'; }
        catch (const std::exception& error) { ++failed; std::cerr << "FAIL " << name << ": " << error.what() << '\n'; }
    }
    std::cout << cases.size() - failed << '/' << cases.size() << " study workload tests; "
              << assertions << " assertions\n";
    return failed ? 1 : 0;
}
