#pragma once

#include "role_browser.h"

namespace amiibo {

class NfcBackend {
public:
    virtual ~NfcBackend() = default;
    // On failure return an error and release any resources acquired during start.
    virtual const char* start(const Image& image) = 0;
    virtual void stop() = 0;
    virtual const char* poll_error() { return nullptr; }
};

class NfcFlow {
public:
    NfcFlow(std::filesystem::path directory, NfcBackend& backend);
    ~NfcFlow() { disable(); }
    NfcFlow(const NfcFlow&) = delete;
    NfcFlow& operator=(const NfcFlow&) = delete;

    void enable();
    void disable();
    void stop_session();
    const char* confirm();
    const char* poll_error()
    {
        if (listening_) {
            if (const auto* error = backend_.poll_error()) {
                stop_session();
                return error;
            }
        }
        return nullptr;
    }

    bool enabled() const { return roles_.has_value(); }
    bool listening() const { return listening_; }
    RoleBrowser& roles() { return roles_.value(); }

private:
    std::filesystem::path directory_;
    NfcBackend& backend_;
    std::optional<RoleBrowser> roles_;
    bool listening_ = false;
};

} // namespace amiibo
