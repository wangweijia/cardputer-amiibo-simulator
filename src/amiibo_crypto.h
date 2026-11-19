#pragma once

#include "bin_catalog.h"

namespace amiibo {

using RetailKeys = std::array<std::uint8_t, 160>;
using PlainImage = std::array<std::uint8_t, 520>;
using Uid = std::array<std::uint8_t, 7>;

class AmiiboCrypto {
public:
    ~AmiiboCrypto();
    AmiiboCrypto() = default;
    AmiiboCrypto(const AmiiboCrypto&) = delete;
    AmiiboCrypto& operator=(const AmiiboCrypto&) = delete;

    const char* load(const std::filesystem::path& directory);
    bool set_keys(const RetailKeys& keys);
    bool unpack(const Image& image, PlainImage& plain) const;
    // Updates the authenticated 520-byte region; preserves the configuration tail.
    bool pack(const PlainImage& plain, Image& image) const;
    const char* rebind(const Image& source, const Uid& uid, Image& output) const;
    const char* new_identity(const Image& source, Image& output) const;

private:
    RetailKeys keys_{};
    bool ready_ = false;
};

} // namespace amiibo
