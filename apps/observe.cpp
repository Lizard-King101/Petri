#include "observation.hpp"

#include <charconv>
#include <chrono>
#include <iostream>
#include <string_view>
#include <thread>

#ifdef _WIN32
#include <conio.h>
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <poll.h>
#include <unistd.h>
#endif

namespace {

// Reads the command bytes waiting on standard input without blocking. Returns
// the count, 0 when nothing is waiting, or -1 at end of input.
int read_waiting(char* bytes, int capacity) {
#ifdef _WIN32
    const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    DWORD size = 0;
    switch (GetFileType(input)) {
    case FILE_TYPE_PIPE: {
        DWORD waiting = 0;
        // Peeking fails once the writer has closed its end.
        if (!PeekNamedPipe(input, nullptr, 0, nullptr, &waiting, nullptr)) {
            return -1;
        }
        if (!waiting) {
            return 0;
        }
        const DWORD take = waiting < static_cast<DWORD>(capacity) ? waiting : static_cast<DWORD>(capacity);
        return ReadFile(input, bytes, take, &size, nullptr) && size ? static_cast<int>(size) : -1;
    }
    case FILE_TYPE_CHAR: {
        // A console: take the keys already pressed, one at a time.
        int count = 0;
        while (count < capacity && _kbhit()) {
            bytes[count++] = static_cast<char>(_getch());
        }
        return count;
    }
    default:
        return ReadFile(input, bytes, static_cast<DWORD>(capacity), &size, nullptr) && size ? static_cast<int>(size)
                                                                                         : -1;
    }
#else
    pollfd input{STDIN_FILENO, POLLIN, 0};
    if (poll(&input, 1, 0) <= 0) {
        return 0;
    }
    const auto size = read(STDIN_FILENO, bytes, static_cast<std::size_t>(capacity));
    return size > 0 ? static_cast<int>(size) : -1;
#endif
}

} // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    // Observations are binary: keep the C runtime from translating line endings.
    _setmode(_fileno(stdout), _O_BINARY);
#endif
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
    petri::LocalPanel panel(world);
    petri::PanelInput commands;
    using Clock = std::chrono::steady_clock;
    const auto start = Clock::now();
    auto next_tick = start + std::chrono::seconds(1);
    while (std::cout) {
        const auto now = Clock::now();
        const auto us = std::chrono::duration_cast<std::chrono::microseconds>(now - start).count();
        const auto ms = static_cast<std::uint32_t>(us / 1000);
        bool requested = false;
        char bytes[256];
        const int size = read_waiting(bytes, sizeof(bytes));
        if (size < 0) {
            return 0;
        }
        for (int i = 0; i < size; ++i) {
            requested |= commands.feed(bytes[i], panel, ms);
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
