#pragma once

#include "role_browser.h"

namespace amiibo {

class NfcBackend {
public:
    virtual ~NfcBackend() = default;
    // Initialize and reserve real hardware before exposing the role browser.
    virtual const char* initialize() = 0;
    // On failure return an error; the flow stops and releases the backend.
    virtual const char* start(const Image& image) = 0;
    virtual void stop_session() = 0;
    virtual void stop() = 0;
    virtual const char* poll_error() { return nullptr; }
    virtual const char* status() { return nullptr; }
    virtual void request_next_identity() {}
};

class NfcFlow {
public:
    NfcFlow(std::filesystem::path directory, NfcBackend& backend);
    ~NfcFlow() { disable(); }
    NfcFlow(const NfcFlow&) = delete;
    NfcFlow& operator=(const NfcFlow&) = delete;

    const char* enable();
    void disable();
    void stop_session();
    const char* confirm();
    const char* poll_error()
    {
        if (enabled()) {
            if (const auto* error = backend_.poll_error()) {
                disable();
                return error;
            }
        }
        return nullptr;
    }

    bool enabled() const { return roles_.has_value(); }
    bool listening() const { return listening_; }
    const char* status() { return listening_ ? backend_.status() : nullptr; }
    void request_next_identity() { if (listening_) { backend_.request_next_identity(); } }
    RoleBrowser& roles() { return roles_.value(); }

private:
    std::filesystem::path directory_;
    NfcBackend& backend_;
    std::optional<RoleBrowser> roles_;
    bool listening_ = false;
};

} // namespace amiibo
