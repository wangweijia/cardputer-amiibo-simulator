#pragma once

#include "bin_catalog.h"
#include <algorithm>
#include <bitset>
#include <cstddef>

namespace amiibo {

struct NtagSession {
    bool authenticated = false;
    bool dirty = false;
    unsigned auth_failures = 0;
    unsigned read_counter = 0;
    bool counted_this_rf = false;
    int pending_write = -1;
    std::bitset<130> read_pages;
    void reset_rf() { authenticated = false; pending_write = -1; counted_this_rf = false; }
};

struct NtagReply {
    std::array<std::uint8_t, 540> data{};
    std::size_t length = 0;
    unsigned valid_bits = 0; // 4 for ACK/NAK, otherwise length * 8 with CRC.
    bool halt = false;
    bool synthetic_signature = false;
    bool read_access = false;
    std::bitset<130> read_pages; // Pages carried by this response only.
};

inline NtagReply ntag_nibble(std::uint8_t value)
{
    NtagReply reply;
    reply.data[0] = value;
    reply.length = 1;
    reply.valid_bits = 4;
    return reply;
}

inline bool ntag_page_locked(const Image& image, unsigned page)
{
    if (page < 4 || page >= 130) { return true; }
    if (page < 8) { return image[10] & (1u << page); }
    if (page < 16) { return image[11] & (1u << (page - 8)); }
    return image[520] & (1u << ((page - 16) / 16));
}

inline NtagReply ntag_write(Image& image, NtagSession& state, unsigned page, const std::uint8_t* data)
{
    // Only application pages are writable. Lock/configuration and UID remain immutable.
    if (ntag_page_locked(image, page)) { return ntag_nibble(0); }
    if (!state.authenticated && page >= image[527]) { return ntag_nibble(4); }
    std::copy_n(data, 4, image.begin() + 4 * page);
    state.dirty = true;
    return ntag_nibble(0x0a);
}

// Hardware may retain or strip the two CRC bytes; accept either frame size.
inline NtagReply ntag_reply(Image& image, NtagSession& state,
                           const std::uint8_t* request, std::size_t size)
{
    NtagReply reply;
    if (!request || size == 0) { return reply; }
    if (state.pending_write >= 0) {
        const auto page = static_cast<unsigned>(state.pending_write);
        state.pending_write = -1;
        return size == 16 || size == 18 ? ntag_write(image, state, page, request) : ntag_nibble(0);
    }
    const auto frame_size = [size](std::size_t expected) { return size == expected || size == expected + 2; };
    switch (request[0]) {
    case 0x30: // READ
    case 0x3a: { // FAST_READ, including a full 135-page image.
        const bool fast = request[0] == 0x3a;
        if (!frame_size(fast ? 3 : 2)) { return ntag_nibble(0); }
        const unsigned first = request[1];
        const unsigned last = fast ? request[2] : first + 3;
        if (first >= 135 || (fast && (last >= 135 || first > last))) { return ntag_nibble(0); }
        const auto pages = last - first + 1;
        for (unsigned n = 0; n < pages; ++n) {
            const auto page = (first + n) % 135;
            if ((image[528] & 0x80) && !state.authenticated && page >= image[527]) {
                return ntag_nibble(4);
            }
        }
        reply.length = pages * 4;
        reply.read_access = true;
        if ((image[528] & 0x10) && !state.counted_this_rf && state.read_counter < 0xffffff) {
            ++state.read_counter;
            state.counted_this_rf = true;
        }
        for (unsigned n = 0; n < pages; ++n) {
            const auto page = (first + n) % 135;
            // PWD and PACK are never exposed through memory READ, even after authentication.
            if (page < 133) {
                std::copy_n(image.begin() + 4 * page, 4, reply.data.begin() + 4 * n);
            }
            if (page < state.read_pages.size()) {
                state.read_pages.set(page);
                reply.read_pages.set(page);
            }
        }
        break;
    }
    case 0x60: // GET_VERSION: NTAG215
        if (!frame_size(1)) { return ntag_nibble(0); }
        {
            const std::array<std::uint8_t, 8> version{0, 4, 4, 2, 1, 0, 0x11, 3};
            std::copy(version.begin(), version.end(), reply.data.begin());
            reply.length = version.size();
        }
        break;
    case 0x3c: // READ_SIG: memory BIN contains no factory ECC signature.
        if (!frame_size(2) || request[1] != 0) { return ntag_nibble(0); }
        reply.length = 32;
        reply.synthetic_signature = true;
        break;
    case 0x50: // HLTA: no response, wait for WUPA or field reset.
        if (!frame_size(2) || request[1] != 0) { return ntag_nibble(0); }
        reply.halt = true;
        state.reset_rf();
        break;
    case 0x1b: { // PWD_AUTH
        if (!frame_size(5)) { return ntag_nibble(0); }
        const unsigned limit = image[528] & 7;
        if (limit && state.auth_failures >= limit) { return ntag_nibble(4); }
        unsigned difference = 0;
        for (unsigned i = 0; i < 4; ++i) { difference |= request[i + 1] ^ image[532 + i]; }
        state.authenticated = difference == 0;
        if (!state.authenticated) { ++state.auth_failures; return ntag_nibble(4); }
        state.auth_failures = 0;
        std::copy_n(image.begin() + 536, 2, reply.data.begin());
        reply.length = 2;
        break;
    }
    case 0xa2: // WRITE: changes only the in-memory session.
        if (!frame_size(6)) { return ntag_nibble(0); }
        return ntag_write(image, state, request[1], request + 2);
    case 0xa0: // COMPATIBILITY_WRITE, first of two frames.
        if (!frame_size(2) || ntag_page_locked(image, request[1])) { return ntag_nibble(0); }
        if (!state.authenticated && request[1] >= image[527]) { return ntag_nibble(4); }
        state.pending_write = request[1];
        return ntag_nibble(0x0a);
    case 0x39: // READ_CNT: NTAG21x exposes counter 2.
        if (!frame_size(2) || request[1] != 2) { return ntag_nibble(0); }
        if (!(image[528] & 0x10)) { return ntag_nibble(0); }
        if ((image[528] & 8) && !state.authenticated) { return ntag_nibble(4); }
        reply.length = 3;
        for (unsigned i = 0; i < 3; ++i) { reply.data[i] = (state.read_counter >> (8 * i)) & 255; }
        break;
    default:
        return ntag_nibble(0);
    }
    return reply;
}

// Convenient stateless wrapper for read-only protocol tools and fixtures.
inline NtagReply ntag_reply(const Image& image, const std::uint8_t* request, std::size_t size)
{
    auto snapshot = image;
    NtagSession state;
    return ntag_reply(snapshot, state, request, size);
}

} // namespace amiibo
