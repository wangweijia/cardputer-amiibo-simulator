#pragma once

#include "nfc_flow.h"

#include <memory>

namespace amiibo {

class CapListener final : public NfcBackend {
public:
    CapListener();
    ~CapListener() override;
    const char* start(const Image& image) override;
    void stop() override;
    const char* poll_error() override;

private:
    struct Session;
    std::unique_ptr<Session> session_;
};

int run_cap_probe();

} // namespace amiibo
