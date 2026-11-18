#pragma once

#include <cstdint>

namespace petri::selftest {

// Board services the self-test needs. The native harness and the ESP32
// app supply their own clock, output, and scheduler yield.
struct Host {
    std::uint64_t (*now_us)();
    void (*print)(const char* line);
    // Called about every 20 ms of stepping so an RTOS can run its idle task.
    void (*yield)();
};

// Runs fixed configurations, printing one `FP ...` line per checkpoint and
// `TIMING ...` / `RENDER ...` lines with measured costs. FP lines must match
// byte for byte between builds of the same world size.
void run(const Host& host);

} // namespace petri::selftest
