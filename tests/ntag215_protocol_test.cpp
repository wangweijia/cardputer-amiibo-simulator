#include "ntag215_protocol.h"
#include <cstdlib>
#include <iostream>

void require(bool ok, const char* message)
{
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}

int main()
{
    amiibo::Image image{};
    for (std::size_t i = 0; i < image.size(); ++i) { image[i] = i & 0xff; }
    image[527] = 0xff; // Unprotected by default.
    image[528] = 0;
    const std::uint8_t signature[]{0x3c, 0, 0, 0};
    for (const auto size : {2u, 4u}) {
        const auto reply = amiibo::ntag_reply(image, signature, size);
        require(reply.length == 32 && reply.synthetic_signature && !reply.halt &&
                std::all_of(reply.data.begin(), reply.data.end(), [](auto b) { return b == 0; }),
                "READ_SIG simulation placeholder changed");
    }
    const std::uint8_t bad_signature[]{0x3c, 1};
    require(amiibo::ntag_reply(image, bad_signature, 2).valid_bits == 4 &&
            amiibo::ntag_reply(image, signature, 1).valid_bits == 4, "Malformed READ_SIG accepted");
    const std::uint8_t halt[]{0x50, 0, 0, 0};
    for (const auto size : {2u, 4u}) {
        const auto reply = amiibo::ntag_reply(image, halt, size);
        require(reply.halt && reply.length == 0, "HLTA must sleep without transmitting");
    }
    const std::uint8_t bad_halt[]{0x50, 1};
    require(!amiibo::ntag_reply(image, bad_halt, 2).halt &&
            !amiibo::ntag_reply(image, halt, 1).halt, "Malformed HLTA accepted");
    const std::uint8_t version[]{0x60, 0, 0};
    for (const auto size : {1u, 3u}) {
        const auto reply = amiibo::ntag_reply(image, version, size);
        require(reply.length == 8 && reply.data[6] == 0x11 && reply.data[7] == 3, "GET_VERSION changed");
    }
    const std::uint8_t read[]{0x30, 134};
    const auto reply = amiibo::ntag_reply(image, read, 2);
    require(reply.length == 16 && reply.data[0] == 0 && reply.data[4] == image[0],
            "READ rollover or hidden PACK changed");
    const std::uint8_t fast[]{0x3a, 0, 134};
    amiibo::NtagSession state;
    const auto full = amiibo::ntag_reply(image, state, fast, 3);
    require(full.length == 540 && full.read_access && state.read_pages.all(), "Full FAST_READ failed");
    require(std::all_of(full.data.begin() + 532, full.data.end(), [](auto b) { return b == 0; }),
            "PWD/PACK leaked in memory reads");
    const std::uint8_t too_large[]{0x3a, 0, 135};
    require(amiibo::ntag_reply(image, too_large, 3).valid_bits == 4, "Out-of-bounds FAST_READ accepted");
    const std::uint8_t reversed[]{0x3a, 10, 9};
    require(amiibo::ntag_reply(image, reversed, 3).valid_bits == 4, "Reversed range accepted");
    require(!amiibo::ntag_reply(image, nullptr, 0).length, "Empty request accepted");

    image[10] = image[11] = image[520] = 0;
    image[527] = 4; // Protect application writes.
    image[528] = 0x80; // Protect reads too.
    image[532] = 1; image[533] = 2; image[534] = 3; image[535] = 4;
    image[536] = image[537] = 0x80;
    state = {};
    const std::uint8_t app_read[]{0x30, 4};
    require(amiibo::ntag_reply(image, state, app_read, 2).data[0] == 4, "Unauthenticated protected READ allowed");
    const std::uint8_t wrong_password[]{0x1b, 0, 0, 0, 0};
    require(amiibo::ntag_reply(image, state, wrong_password, 5).valid_bits == 4 && !state.authenticated,
            "Incorrect password accepted");
    const std::uint8_t password[]{0x1b, 1, 2, 3, 4, 0, 0};
    const auto pack = amiibo::ntag_reply(image, state, password, 7);
    require(pack.length == 2 && pack.valid_bits == 0 && pack.data[0] == 0x80 && state.authenticated,
            "Password authentication failed");
    require(amiibo::ntag_reply(image, state, app_read, 2).length == 16, "Authenticated READ failed");
    const std::uint8_t write[]{0xa2, 4, 9, 8, 7, 6, 0, 0};
    require(amiibo::ntag_reply(image, state, write, 8).data[0] == 0x0a && image[16] == 9 && state.dirty,
            "Authenticated RAM WRITE failed");
    image[10] |= 1 << 4;
    require(amiibo::ntag_reply(image, state, write, 6).data[0] == 0, "Static page lock ignored");
    image[520] = 1;
    const std::uint8_t dynamic_write[]{0xa2, 16, 1, 2, 3, 4};
    require(amiibo::ntag_reply(image, state, dynamic_write, 6).data[0] == 0, "Dynamic page lock ignored");
    const std::uint8_t compat[]{0xa0, 32};
    require(amiibo::ntag_reply(image, state, compat, 2).data[0] == 0x0a, "Compatibility write stage one failed");
    const std::uint8_t body[18]{0x19, 0x18, 0x17, 0x16};
    require(amiibo::ntag_reply(image, state, body, 18).data[0] == 0x0a && image[128] == 0x19,
            "Compatibility write continuation failed");
    state.reset_rf();
    require(!state.authenticated && state.pending_write == -1, "RF reset retained authentication");
    require(amiibo::ntag_reply(image, state, write, 6).valid_bits == 4, "Unauthenticated WRITE allowed");
    image[528] = 0x18; // Enable counter and protect its value with PWD.
    const std::uint8_t counter[]{0x39, 2};
    require(amiibo::ntag_reply(image, state, counter, 2).valid_bits == 4,
            "Protected counter exposed without authentication");
    amiibo::ntag_reply(image, state, password, 7);
    amiibo::ntag_reply(image, state, app_read, 2);
    amiibo::ntag_reply(image, state, app_read, 2);
    const auto count = amiibo::ntag_reply(image, state, counter, 2);
    require(count.length == 3 && count.data[0] == 1, "Counter must increment only once per RF session");
    // Short and malformed frames must never read outside their bounds.
    for (unsigned opcode = 0; opcode < 256; ++opcode) {
        const auto byte = static_cast<std::uint8_t>(opcode);
        amiibo::ntag_reply(image, state, &byte, 1);
    }
}
