#pragma once

#include "nfc_flow.h"
#include "amiibo_crypto.h"

#include <memory>

namespace amiibo {

class CapListener final : public NfcBackend {
public:
    explicit CapListener(std::filesystem::path directory = {});
    ~CapListener() override;
    const char* initialize() override;
    const char* start(const Image& image) override;
    void stop_session() override;
    void stop() override;
    const char* poll_error() override;
    const char* status() override;
    void request_next_identity() override;

private:
    struct Session;
    std::filesystem::path directory_;
    std::unique_ptr<AmiiboCrypto> crypto_;
    std::unique_ptr<Session> session_;
    char status_text_[128]{};
};

int run_cap_probe();

} // namespace amiibo
