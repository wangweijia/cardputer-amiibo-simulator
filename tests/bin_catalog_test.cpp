#include "bin_catalog.h"
#include "nfc_flow.h"
#include "role_browser.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>

#include <unistd.h>

namespace fs = std::filesystem;

int main()
{
    auto name = (fs::temp_directory_path() / "amiibozero-test-XXXXXX").string();
    if (!mkdtemp(name.data())) {
        std::cerr << "Could not create fixture directory\n";
        return 1;
    }
    struct FixtureDirectory {
        fs::path path;
        ~FixtureDirectory()
        {
            std::error_code error;
            fs::remove_all(path, error);
        }
    } dir{name};

    amiibo::Image data{0x04, 0x11, 0x22, 0xBF, 0x33, 0x44, 0x55, 0x66, 0x44};
    const fs::path role = dir.path / "\u738b\u56fd\u4e4b\u6cea\uff0d\u6797\u514b.BIN";
    const auto write_image = [&](const fs::path& path, const amiibo::Image& image, std::size_t size) {
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(image.data()), size);
        out.close();
        return out.good();
    };
    auto corrupt = data;
    corrupt[8] ^= 1;
    if (!write_image(role, data, data.size()) ||
        !write_image(dir.path / "RETAIL.BIN", data, data.size()) ||
        !write_image(dir.path / "key_retail.bin", data, 160) ||
        !write_image(dir.path / "short.bin", data, 539) ||
        !write_image(dir.path / "corrupt.bin", corrupt, corrupt.size())) {
        std::cerr << "Could not write synthetic fixtures\n";
        return 1;
    }
    fs::create_directory(dir.path / "directory.bin");
    fs::create_symlink(role, dir.path / "link.bin");

    const auto catalog = amiibo::scan_bins(dir.path);
    const auto contains = [&](const fs::path& path, amiibo::BinStatus status) {
        return std::any_of(catalog.entries.begin(), catalog.entries.end(), [&](const auto& entry) {
            return entry.path == path && entry.status == status;
        });
    };
    if (catalog.error || catalog.entries.size() != 3 ||
        !contains(role, amiibo::BinStatus::valid) ||
        !contains(dir.path / "short.bin", amiibo::BinStatus::wrong_size) ||
        !contains(dir.path / "corrupt.bin", amiibo::BinStatus::invalid_header)) {
        std::cerr << "Unexpected catalog filtering or validation result\n";
        return 1;
    }
    const auto selected = amiibo::load_bin(role);
    if (selected.image != data ||
        amiibo::load_bin(dir.path / "RETAIL.BIN").image ||
        amiibo::load_bin(dir.path / "link.bin").image ||
        !amiibo::scan_bins(dir.path / "missing").error) {
        std::cerr << "Selection accepted an invalid input or returned incorrect data\n";
        return 1;
    }

    amiibo::RoleBrowser empty(amiibo::Catalog{});
    if (empty.move('x') || empty.confirm()) {
        std::cerr << "Empty browser allowed navigation or selection\n";
        return 1;
    }

    for (const auto* filename : {"a.bin", "b.bin", "c.bin"}) {
        if (!write_image(dir.path / filename, data, data.size())) {
            std::cerr << "Could not write pagination fixtures\n";
            return 1;
        }
    }
    amiibo::RoleBrowser browser(amiibo::scan_bins(dir.path));
    if (browser.confirm() != amiibo::BinStatus::valid ||
        browser.selected_path() != dir.path / "a.bin" || browser.selected_image() != data ||
        !browser.move('c') || browser.focus() != 4 ||
        browser.confirm() != amiibo::BinStatus::wrong_size ||
        browser.selected_path() != dir.path / "a.bin" || browser.selected_image() != data ||
        !browser.move('x') || browser.confirm() != amiibo::BinStatus::valid ||
        browser.selected_path() != role ||
        browser.move('x') || !browser.move('f') || !browser.move('z') || browser.focus() != 0) {
        std::cerr << "Role browser paging, selection, or invalid-file rejection failed\n";
        return 1;
    }
    if (!write_image(dir.path / "a.bin", corrupt, corrupt.size()) ||
        browser.confirm() != amiibo::BinStatus::invalid_header ||
        browser.selected_path() != role || browser.selected_image() != data) {
        std::cerr << "Selection did not revalidate the file or preserve the previous role\n";
        return 1;
    }

    struct Backend : amiibo::NfcBackend {
        const char* error = "Listener unavailable";
        int starts = 0;
        int stops = 0;
        std::optional<amiibo::Image> received;
        const char* start(const amiibo::Image& image) override
        {
            ++starts;
            received = image;
            return error;
        }
        void stop() override { ++stops; }
    } backend;

    {
        amiibo::NfcFlow flow(dir.path, backend);
        if (flow.enabled() || !flow.confirm() || backend.starts != 0) {
            std::cerr << "NFC was active before explicit enable\n";
            return 1;
        }
        flow.enable();
        if (!flow.confirm() || backend.starts != 0) {
            std::cerr << "Invalid BIN reached NFC backend\n";
            return 1;
        }
        flow.roles().highlight(role.filename());
        if (!flow.confirm() || flow.listening() || backend.starts != 1 || backend.received != data) {
            std::cerr << "Valid confirmation did not reach backend or failure was hidden\n";
            return 1;
        }
    }
    {
        amiibo::NfcFlow flow(dir.path, backend);
        flow.enable();
        const auto& roles = flow.roles();
        if (roles.catalog().entries[roles.focus()].path != role || flow.listening() || backend.starts != 1) {
            std::cerr << "Last role should be highlighted but not automatically started\n";
            return 1;
        }
        backend.error = nullptr;
        if (flow.confirm() || !flow.listening() || backend.starts != 2) {
            std::cerr << "Confirm did not start a session\n";
            return 1;
        }
        flow.disable();
        if (flow.enabled() || backend.stops != 1) {
            std::cerr << "Manual disable did not stop the session\n";
            return 1;
        }
        flow.enable();
        if (flow.confirm() || !flow.listening()) {
            return 1;
        }
    }
    fs::remove(role);
    amiibo::NfcFlow flow(dir.path, backend);
    flow.enable();
    if (backend.starts != 3 || backend.stops != 2 || flow.roles().focus() != 0) {
        std::cerr << "Exit did not stop NFC or a missing saved role changed default focus\n";
        return 1;
    }
}
