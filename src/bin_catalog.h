#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <system_error>
#include <vector>

namespace amiibo {

using Image = std::array<std::uint8_t, 540>;

enum class BinStatus { valid, key_file, unsupported, unreadable, wrong_size, invalid_header };

struct BinResult {
    BinStatus status;
    std::optional<Image> image;
};

struct BinEntry {
    std::filesystem::path path;
    BinStatus status;
};

struct Catalog {
    std::vector<BinEntry> entries;
    std::error_code error;
};

BinResult load_bin(const std::filesystem::path& path);
Catalog scan_bins(const std::filesystem::path& directory);
const char* describe(BinStatus status);

} // namespace amiibo
