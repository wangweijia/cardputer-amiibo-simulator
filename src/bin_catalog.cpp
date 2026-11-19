#include "bin_catalog.h"

#include <algorithm>
#include <cerrno>
#include <string>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace amiibo {
namespace {

std::string lowercase_ascii(std::string text)
{
    for (auto& ch : text) {
        if (ch >= 'A' && ch <= 'Z') {
            ch += 'a' - 'A';
        }
    }
    return text;
}

bool has_bin_extension(const std::filesystem::path& path)
{
    return lowercase_ascii(path.extension().string()) == ".bin";
}

} // namespace

BinResult load_bin(const std::filesystem::path& path)
{
    if (!has_bin_extension(path)) {
        return {BinStatus::unsupported, std::nullopt};
    }
    const auto name = lowercase_ascii(path.filename().string());
    if (name == "key_retail.bin" || name == "retail.bin") {
        return {BinStatus::key_file, std::nullopt};
    }

    // Do not follow links or block on special files in the catalog.
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) {
        return {BinStatus::unreadable, std::nullopt};
    }
    Image image{};
    struct stat info{};
    auto status = BinStatus::unreadable;
    if (fstat(fd, &info) == 0 && S_ISREG(info.st_mode)) {
        if (info.st_size != static_cast<off_t>(image.size())) {
            status = BinStatus::wrong_size;
        } else {
            std::size_t offset = 0;
            while (offset < image.size()) {
                const auto n = read(fd, image.data() + offset, image.size() - offset);
                if (n > 0) {
                    offset += static_cast<std::size_t>(n);
                } else if (n < 0 && errno == EINTR) {
                    continue;
                } else {
                    break;
                }
            }
            if (offset == image.size()) {
                std::uint8_t extra;
                ssize_t n;
                do {
                    n = read(fd, &extra, sizeof(extra));
                } while (n < 0 && errno == EINTR);
                status = n == 0 ? BinStatus::valid
                                : n > 0 ? BinStatus::wrong_size : BinStatus::unreadable;
            }
        }
    }
    close(fd);
    if (status != BinStatus::valid) {
        return {status, std::nullopt};
    }

    const auto bcc0 = 0x88 ^ image[0] ^ image[1] ^ image[2];
    const auto bcc1 = image[4] ^ image[5] ^ image[6] ^ image[7];
    if (image[0] != 0x04 || image[3] != bcc0 || image[8] != bcc1) {
        return {BinStatus::invalid_header, std::nullopt};
    }
    return {BinStatus::valid, image};
}

Catalog scan_bins(const std::filesystem::path& directory)
{
    Catalog result;
    std::filesystem::directory_iterator entry(directory, result.error), end;
    for (; !result.error && entry != end; entry.increment(result.error)) {
        const auto info = entry->symlink_status(result.error);
        if (!result.error && std::filesystem::is_regular_file(info) &&
            has_bin_extension(entry->path())) {
            const auto bin = load_bin(entry->path());
            if (bin.status != BinStatus::key_file) {
                result.entries.push_back({entry->path(), bin.status});
            }
        }
    }
    std::sort(result.entries.begin(), result.entries.end(), [](const auto& a, const auto& b) {
        return a.path.filename() < b.path.filename();
    });
    return result;
}

const char* describe(BinStatus status)
{
    switch (status) {
    case BinStatus::valid: return "passed basic checks";
    case BinStatus::key_file: return "key file, not a role BIN";
    case BinStatus::unsupported: return "expected a .bin file";
    case BinStatus::unreadable: return "cannot read a regular BIN file";
    case BinStatus::wrong_size: return "expected exactly 540 bytes";
    case BinStatus::invalid_header: return "invalid NXP UID header or check bytes";
    }
    return "unknown BIN status";
}

} // namespace amiibo
