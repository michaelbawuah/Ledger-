// Optional GNU gprof driver. It disables sampling during generation/setup and
// repeatedly processes one fixed stream. Not used for headline timing numbers.
#include "ledger/book.hpp"
#include "ledger/reference_book.hpp"
#include "ledger/workload.hpp"
#include <sys/gmon.h>
#include <iostream>
#include <vector>
extern "C" void moncontrol(int);
int main() {
    moncontrol(0);
    const auto commands = ledger::make_workload<ledger::ReferenceBook>(1000000, 42);
    std::vector<ledger::Event> storage(ledger::required_event_capacity(8192));
    ledger::EventBuffer output(storage);
    std::uint64_t checksum = 0;
    for (int run = 0; run < 20; ++run) {
        ledger::FastBook book;
        moncontrol(1);
        for (const auto& command : commands) {
            book.process(command, output);
            for (const auto& event : output.events()) checksum += event.id + event.quantity;
        }
        moncontrol(0);
    }
    std::cout << "20 million profiled fast-engine commands; event checksum=" << checksum << '\n';
}
