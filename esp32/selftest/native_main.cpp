// Native build of the ESP32 self-test, used to produce the reference FP lines.
#include "selftest.hpp"

#include <chrono>
#include <cstdio>

int main() {
    petri::selftest::run({
        [] {
            using namespace std::chrono;
            return std::uint64_t(duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());
        },
        [](const char* line) { std::puts(line); },
        [] {},
    });
}
