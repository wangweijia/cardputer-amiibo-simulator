// Explicit device test: no private data in output, no file modifications.
#include "cap_listener.h"
#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>

int main(int argc, char** argv)
{
    if (argc != 2) { return 2; }
    auto catalog = amiibo::scan_bins(argv[1]);
    if (catalog.error || catalog.entries.empty()) { return 1; }
    auto source = amiibo::load_bin(catalog.entries.front().path);
    if (!source.image) { return 1; }
    amiibo::CapListener backend(argv[1]);
    if (const auto* error = backend.initialize()) { std::cerr << error << '\n'; return 1; }
    if (const auto* error = backend.start(*source.image)) { std::cerr << error << '\n'; return 1; }
    const auto at_generation = [&](const char* marker) {
        const auto* status = backend.status();
        return !backend.poll_error() && status && std::strstr(status, marker);
    };
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    if (!at_generation("UID #1")) { return 3; }
    backend.request_next_identity();
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    if (!at_generation("UID #2")) { return 4; }
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    if (!at_generation("UID #2")) { return 5; }
    backend.request_next_identity();
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    if (!at_generation("UID #3")) { return 6; }
    backend.stop();
    std::cout << "PASS: hardware UID cycles 1 -> 2 -> 3, idle identity stable, SPI released\n";
}
