#pragma once

#include "ledger/types.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ledger::study {

// A deterministic synthetic experiment. Preload is replayed before timing;
// commands contains exactly the requested measured command count.
struct Workload {
    std::string profile;
    Config config;
    std::vector<Command> preload;
    std::vector<Command> commands;
};

// depth is initial resting orders PER SIDE. Prices occupy [1, domain].
// Valid depth: 1..16384; valid domain: 2*depth..1000000; count: 0..1000000.
// Pool capacity is max(4*depth,1024), bounded at 65536.
// Unsupported profiles or invalid parameters throw std::invalid_argument.
Workload make_workload(std::string_view profile, std::size_t commands,
                       std::uint64_t seed, std::size_t depth = 256,
                       Price domain = 20000);

std::vector<std::string> profiles();

} // namespace ledger::study
