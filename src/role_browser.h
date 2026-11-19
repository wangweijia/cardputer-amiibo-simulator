#pragma once

#include "bin_catalog.h"

#include <utility>

namespace amiibo {

class RoleBrowser {
public:
    static constexpr std::size_t page_size = 4;

    explicit RoleBrowser(Catalog catalog) : catalog_(std::move(catalog)) {}

    const Catalog& catalog() const { return catalog_; }
    std::size_t focus() const { return focus_; }
    std::size_t page_start() const { return focus_ / page_size * page_size; }
    const std::optional<Image>& selected_image() const { return selected_; }
    const std::filesystem::path& selected_path() const { return selected_path_; }

    void highlight(const std::filesystem::path& filename)
    {
        for (std::size_t i = 0; i < catalog_.entries.size(); ++i) {
            const auto& entry = catalog_.entries[i];
            if (entry.status == BinStatus::valid && entry.path.filename() == filename) {
                focus_ = i;
                break;
            }
        }
    }

    bool move(char key)
    {
        const auto count = catalog_.entries.size();
        if (count == 0) {
            return false;
        }
        const auto old = focus_;
        switch (key) {
        case 'f': if (focus_ > 0) --focus_; break;
        case 'x': if (focus_ + 1 < count) ++focus_; break;
        case 'z': if (page_start() > 0) focus_ = page_start() - page_size; break;
        case 'c':
            if (count - page_start() > page_size) {
                focus_ = page_start() + page_size;
            }
            break;
        }
        return old != focus_;
    }

    std::optional<BinStatus> confirm()
    {
        if (catalog_.entries.empty()) {
            return std::nullopt;
        }
        auto& entry = catalog_.entries[focus_];
        auto bin = load_bin(entry.path);
        entry.status = bin.status;
        if (bin.image) {
            selected_ = std::move(bin.image);
            selected_path_ = entry.path;
        }
        return bin.status;
    }

private:
    Catalog catalog_;
    std::optional<Image> selected_;
    std::filesystem::path selected_path_;
    std::size_t focus_ = 0;
};

} // namespace amiibo
