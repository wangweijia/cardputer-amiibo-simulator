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
        const char* init_error = "NFC initialization failed";
        const char* error = nullptr;
        const char* runtime_error = nullptr;
        int initializations = 0;
        int starts = 0;
        bool ready = false;
        bool active = false;
        std::optional<amiibo::Image> received;
        const char* initialize() override
        {
            ++initializations;
            ready = !init_error;
            return init_error;
        }
        const char* start(const amiibo::Image& image) override
        {
            ++starts;
            received = image;
            active = !error;
            return error;
        }
        void stop_session() override { active = false; }
        void stop() override { active = ready = false; }
        const char* poll_error() override { return runtime_error; }
    } backend;

    {
        amiibo::NfcFlow flow(dir.path, backend);
        if (flow.enabled() || !flow.confirm() || backend.starts != 0 ||
            !flow.enable() || flow.enabled() || backend.ready) {
            std::cerr << "Failed initialization exposed the BIN browser\n";
            return 1;
        }
        backend.init_error = nullptr;
        if (flow.enable() || !flow.enabled() || !backend.ready || backend.starts != 0) {
            std::cerr << "Browser did not wait for hardware initialization\n";
            return 1;
        }
        const auto initializations = backend.initializations;
        if (flow.enable() || backend.initializations != initializations ||
            !flow.confirm() || backend.starts != 0) {
            std::cerr << "Enable was not idempotent or invalid BIN reached backend\n";
            return 1;
        }
        flow.roles().highlight(role.filename());
        backend.error = "Listener unavailable";
        if (!flow.confirm() || flow.enabled() || backend.ready || backend.starts != 1 ||
            backend.received != data) {
            std::cerr << "Failed listener start did not return to disabled state\n";
            return 1;
        }
        backend.error = nullptr;
        if (flow.enable() || flow.roles().catalog().entries[flow.roles().focus()].path != role ||
            flow.listening() || backend.starts != 1 || flow.confirm() || !flow.listening()) {
            std::cerr << "Role confirmation or last-role restore failed\n";
            return 1;
        }
        flow.stop_session();
        if (!flow.enabled() || !backend.ready || backend.active || flow.listening() ||
            flow.confirm() || !flow.listening()) {
            std::cerr << "Returning to browser lost initialized hardware\n";
            return 1;
        }
        backend.runtime_error = "SPI disconnected";
        if (!flow.poll_error() || flow.enabled() || backend.ready || backend.active) {
            std::cerr << "Hardware failure left the browser enabled\n";
            return 1;
        }
        backend.runtime_error = nullptr;
        if (flow.enable()) { return 1; }
        flow.disable();
        if (flow.enabled() || backend.ready) { return 1; }
        if (flow.enable() || flow.confirm()) { return 1; }
    }
    if (backend.ready || backend.active) {
        std::cerr << "Exit did not release NFC hardware\n";
        return 1;
    }
    fs::remove(role);
    amiibo::NfcFlow flow(dir.path, backend);
    if (flow.enable() || flow.roles().focus() != 0 || flow.listening()) {
        std::cerr << "Missing saved role changed default focus or started listening\n";
        return 1;
    }
}
