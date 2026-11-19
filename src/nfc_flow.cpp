#include "nfc_flow.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

#include <unistd.h>

namespace amiibo {
namespace {

constexpr auto last_role = ".amiibozero-last-role";

bool store_role(const std::filesystem::path& directory, const std::string& name)
{
    auto scratch = (directory / ".amiibozero-last-role-XXXXXX").string();
    const int fd = mkstemp(scratch.data());
    if (fd < 0) {
        return false;
    }
    auto* out = fdopen(fd, "wb");
    if (!out) {
        close(fd);
        unlink(scratch.c_str());
        return false;
    }
    const bool written = std::fwrite(name.data(), 1, name.size(), out) == name.size();
    const bool closed = std::fclose(out) == 0;
    if (written && closed && std::rename(scratch.c_str(), (directory / last_role).c_str()) == 0) {
        return true;
    }
    unlink(scratch.c_str());
    return false;
}

} // namespace

NfcFlow::NfcFlow(std::filesystem::path directory, NfcBackend& backend)
    : directory_(std::move(directory)), backend_(backend)
{
}

const char* NfcFlow::enable()
{
    if (enabled()) {
        return nullptr;
    }
    if (const auto* error = backend_.initialize()) {
        backend_.stop();
        return error;
    }
    roles_.emplace(scan_bins(directory_));
    if (roles_->catalog().error) {
        std::fprintf(stderr, "AmiiboZero: cannot scan role directory: %s\n",
                     roles_->catalog().error.message().c_str());
    }
    const auto state_path = directory_ / last_role;
    std::error_code error;
    if (!std::filesystem::is_regular_file(std::filesystem::symlink_status(state_path, error))) {
        return nullptr;
    }
    std::ifstream in(state_path, std::ios::binary);
    char name[256];
    in.read(name, sizeof(name));
    if (in.eof() && !in.bad() && in.gcount() > 0) {
        roles_->highlight(std::string(name, static_cast<std::size_t>(in.gcount())));
    }
    return nullptr;
}

void NfcFlow::disable()
{
    backend_.stop();
    listening_ = false;
    roles_.reset();
}

void NfcFlow::stop_session()
{
    if (listening_) {
        backend_.stop_session();
        listening_ = false;
    }
}

const char* NfcFlow::confirm()
{
    if (!enabled()) {
        return "Enable NFC first.";
    }
    if (listening_) {
        return "Stop the current session first.";
    }
    const auto status = roles_->confirm();
    if (!status) {
        return "No role BIN to select.";
    }
    if (*status != BinStatus::valid) {
        return describe(*status);
    }
    const bool saved = store_role(directory_, roles_->selected_path().filename().string());
    if (!saved) {
        std::fputs("AmiiboZero: could not save last confirmed role\n", stderr);
    }
    const auto* error = backend_.start(*roles_->selected_image());
    if (error) {
        disable();
        return error;
    }
    listening_ = true;
    return saved ? nullptr : "Listening; last role could not be saved.";
}

} // namespace amiibo
