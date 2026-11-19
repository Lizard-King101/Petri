#pragma once

#include <cstddef>
#include <cstdint>

namespace petri::selftest {

// Board services the self-test needs. The native harness and the ESP32
// app supply their own clock, output, and scheduler yield.
struct Host {
    std::uint64_t (*now_us)();
    void (*print)(const char* line);
    // Called about every 20 ms of stepping so an RTOS can run its idle task.
    void (*yield)();
    // Allocates word-aligned memory for the brain benchmark, which touches it
    // only with 32-bit loads (ESP32 instruction RAM allows nothing narrower).
    // Null uses ordinary heap memory. `region` names what was used.
    void* (*alloc_words)(std::size_t bytes, const char** region) = nullptr;
};

// Runs fixed configurations, printing one `FP ...` line per checkpoint and
// `TIMING ...` / `RENDER ...` lines with measured costs. FP lines must match
// byte for byte between builds of the same world size. `BRAIN ...` lines time the
// creature brain in core/brain.cpp (docs/EMERGENT_LIFE.md).
void run(const Host& host);

} // namespace petri::selftest
