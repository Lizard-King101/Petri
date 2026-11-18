#include "observation.hpp"

#include <charconv>
#include <chrono>
#include <iostream>
#include <poll.h>
#include <string_view>
#include <thread>
#include <unistd.h>

int main(int argc, char** argv) {
    std::uint32_t seed = 14;
    if (argc == 2) {
        const std::string_view input = argv[1];
        const auto parsed = std::from_chars(input.data(), input.data() + input.size(), seed);
        if (parsed.ec != std::errc{} || parsed.ptr != input.data() + input.size()) {
            std::cerr << "Usage: petri_observer [seed]\n";
            return 2;
        }
    } else if (argc > 2) {
        std::cerr << "Usage: petri_observer [seed]\n";
        return 2;
    }

    petri::World world(seed, petri::observatory_config());
    for (unsigned tick = 0; tick < 256; ++tick) {
        world.step();
    }

    petri::Observation snapshot;
    petri::Panel panel(world);
    petri::PanelInput commands;
    using Clock = std::chrono::steady_clock;
    const auto start = Clock::now();
    auto next_tick = start + std::chrono::seconds(1);
    while (std::cout) {
        const auto now = Clock::now();
        const auto us = std::chrono::duration_cast<std::chrono::microseconds>(now - start).count();
        const auto ms = static_cast<std::uint32_t>(us / 1000);
        bool requested = false;
        pollfd input{STDIN_FILENO, POLLIN, 0};
        if (poll(&input, 1, 0) > 0) {
            char bytes[256];
            const auto size = read(STDIN_FILENO, bytes, sizeof(bytes));
            if (size <= 0) {
                return 0;
            }
            for (int i = 0; i < size; ++i) {
                requested |= commands.feed(bytes[i], panel, ms);
            }
        }
        if (now >= next_tick) {
            world.step(panel.before_step(ms));
            panel.after_step(us, 1000000);
            next_tick += std::chrono::seconds(1);
        }
        panel.draw(ms, us, 0);
        if (requested) {
            const auto size = petri::capture_observation(world, panel, commands.revision(), snapshot);
            std::cout.write(reinterpret_cast<const char*>(snapshot.data()), size).flush();
        }
        std::this_thread::sleep_until(now + std::chrono::milliseconds(50));
    }
}
