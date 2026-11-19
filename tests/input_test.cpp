#include "device_input.h"

#include <cstdio>

int main()
{
    using namespace std::chrono_literals;
    const DeviceInput::Clock::time_point start{};
    DeviceInput input;
    for (const auto key : {'c', 'x', 'z', 'f'}) {
        input.key(key, true, start);
        const int expected = key == 'c' ? 1 : key == 'x' ? 3 : key == 'z' ? 2 : 0;
        if (input.focused_tile() != expected) {
            std::fprintf(stderr, "Incorrect navigation for %c\n", key);
            return 1;
        }
    }

    input.key('\x1b', true, start);
    input.tick(start + 2999ms);
    if (input.quitting()) {
        std::fputs("Esc exited before three seconds\n", stderr);
        return 1;
    }
    input.key('\x1b', false, start + 2999ms);
    input.tick(start + 4s);
    if (input.quitting() || input.holding()) {
        std::fputs("Releasing Esc early did not cancel exit\n", stderr);
        return 1;
    }

    input.key('\x1b', true, start + 4s);
    input.key('c', true, start + 5s);
    input.key('\x1b', true, start + 6s);
    input.tick(start + 7s);
    if (!input.quitting()) {
        std::fputs("Holding Esc for three seconds did not request exit\n", stderr);
        return 1;
    }
}
