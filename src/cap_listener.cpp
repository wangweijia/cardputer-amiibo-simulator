#include "cap_listener.h"
#include "ntag215_protocol.h"
#include "uid_cycle.h"

#include <array>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <system_error>

#include <fcntl.h>
#include <linux/spi/spidev.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace amiibo {
namespace {

using Byte = std::uint8_t;
using namespace std::chrono_literals;
volatile std::sig_atomic_t stopped = 0;

void interrupt(int) { stopped = 1; }

constexpr Image sample{
    0x04, 0x11, 0x22, 0xbf, 0x33, 0x44, 0x55, 0x66, 0x44, 0x48, 0, 0,
    0xe1, 0x10, 0x3e, 0,
    0x03, 0x10, 0xd1, 0x01, 0x0c, 0x55, 0x04,
    'e', 'x', 'a', 'm', 'p', 'l', 'e', '.', 'c', 'o', 'm', 0xfe,
};

struct Diagnostics {
    unsigned fields = 0;
    unsigned selections = 0;
    unsigned requests = 0;
    unsigned sent = 0;
    unsigned queued = 0;
    unsigned unsupported = 0;
    unsigned errors = 0;
    unsigned last_command = 0;
    unsigned last_irq = 0;
};

class Cap {
public:
    ~Cap()
    {
        if (configured_) {
            const bool stopped_chip = command(0xc2);
            const bool disabled_chip = write(0x02, 0);
            if (!stopped_chip || !disabled_chip) {
                std::fputs("Could not disable the NFC chip\n", stderr);
            }
        }
        if (fd_ >= 0) {
            close(fd_);
        }
    }

    template<std::size_t N>
    bool transfer(const std::array<Byte, N>& tx, std::array<Byte, N>& rx, std::size_t length = N)
    {
        spi_ioc_transfer packet{};
        packet.tx_buf = reinterpret_cast<std::uintptr_t>(tx.data());
        packet.rx_buf = reinterpret_cast<std::uintptr_t>(rx.data());
        packet.len = length;
        packet.speed_hz = 5'000'000;
        packet.bits_per_word = 8;
        const auto count = ioctl(fd_, SPI_IOC_MESSAGE(1), &packet);
        if (count < 0) {
            std::perror("SPI transfer");
        }
        return count == static_cast<int>(length);
    }

    bool open_cap()
    {
        fd_ = open("/dev/spidev0.2", O_RDWR | O_CLOEXEC);
        if (fd_ < 0 || flock(fd_, LOCK_EX | LOCK_NB) < 0) {
            std::perror("open/lock Cap SPI");
            return false;
        }
        Byte mode = SPI_MODE_1;
        if (ioctl(fd_, SPI_IOC_WR_MODE, &mode) != 0) {
            std::perror("configure Cap SPI mode");
            return false;
        }
        std::array<Byte, 2> id{};
        if (!transfer(std::array<Byte, 2>{0x7f, 0}, id)) {
            return false;
        }
        std::printf("ST25R3916 identity: 0x%02x\n", static_cast<unsigned>(id[1]));
        return (id[1] & 0xf8) == 0x28 || (id[1] & 0xf8) == 0x30;
    }

    bool initialize()
    {
        configured_ = true;
        if (!command(0xc1)) {
            return false;
        }
        std::this_thread::sleep_for(2ms);
        if (!write(0x00, 0x07) || !write(0x01, 0x3c) || !write(0x02, 0x80) ||
            !write(0x28, 0xd0)) {
            return false;
        }
        std::this_thread::sleep_for(5ms);

        if (!command(0xd6)) {
            return false;
        }
        std::this_thread::sleep_for(5ms);
        return true;
    }

    bool quiet() { return command(0xc2) && write(0x02, 0x80); }

    bool prepare(const Image& image)
    {
        // NFC-A passive target, 106 kbit/s, seven-byte UID.
        if (!write(0x03, 0x88) || !write(0x04, 0) || !write(0x08, 0x5c) ||
            !write(0x0a, 0x10) || !write(0x0b, 0x08) || !write(0x0c, 0x2d) ||
            !write(0x0d, 0) || !write(0x0e, 0) || !write(0x0f, 0x02) ||
            !write(0x12, 0x08) || !write(0x29, 0x5f) ||
            !write(0x2a, 0x13) || !write(0x2b, 0x02)) {
            return false;
        }

        const std::array<Byte, 16> pt{0xa0, image[0], image[1], image[2],
                                      image[4], image[5], image[6], image[7],
                                      0, 0, 0, 0x44, 0, 0x04, 0, 0};
        std::array<Byte, 16> unused{};
        std::array<Byte, 17> readback{};
        if (!transfer(pt, unused) || !transfer(std::array<Byte, 17>{0xbf}, readback) ||
            !std::equal(pt.begin() + 1, pt.end(), readback.begin() + 2)) {
            std::fputs("Passive-target memory verification failed\n", stderr);
            return false;
        }
        std::array<Byte, 5> status{};
        return write(0x16, 0x47) && write(0x17, 0xe6) && write(0x18, 0x0f) &&
               write(0x19, 0xec) && irq(status) && write(0x02, 0xc3) &&
               command(0xdb) && command(0xd1) && command(0xcd);
    }

    bool irq(std::array<Byte, 5>& flags)
    {
        return transfer(std::array<Byte, 5>{0x5a}, flags);
    }

    bool sense() { return write(0x08, 0x5c) && command(0xcd); }
    bool selected() { return write(0x08, 0x5d); }
    bool rearm() { return command(0xd1); }
    bool discard_frame() { return command(0xdb) && rearm(); }

    bool field_present(bool& present)
    {
        std::array<Byte, 2> status{};
        if (!transfer(std::array<Byte, 2>{0x71, 0}, status)) { return false; }
        present = status[1] & 0x40;
        return true;
    }

    bool respond(Image& image, NtagSession& protocol, bool& read_sent, ReadProgress& progress, bool& active, bool trace, Diagnostics* stats)
    {
        const auto began = std::chrono::steady_clock::now();
        std::array<Byte, 3> fifo{};
        if (!transfer(std::array<Byte, 3>{0x5e}, fifo)) {
            return false;
        }
        const auto count = static_cast<std::size_t>(fifo[1]) | ((fifo[2] & 0xc0u) << 2);
        if (count == 0) {
            return rearm();
        }
        std::array<Byte, 19> request{};
        if (count >= request.size()) {
            return command(0xdb) && command(0xd1);
        }
        if (!transfer(std::array<Byte, 19>{0x9f}, request, count + 1)) {
            return false;
        }
        // Compatibility-write continuation contains data, so log the command, not payload bytes.
        const auto opcode = protocol.pending_write >= 0 ? Byte{0xa0} : request[1];
        if (stats) {
            ++stats->requests;
            stats->last_command = opcode;
        }
        const auto response = ntag_reply(image, protocol, request.data() + 1, count);
        if (response.halt) {
            const bool halted = write(0x08, 0x5c) && command(0xce);
            if (halted) { active = false; }
            if (stats && stats->requests <= 32) {
                std::fprintf(stderr, "NFC request: cmd=50 length=%zu action=halt ok=%d\n", count, int(halted));
            }
            return halted;
        }
        const auto bytes = response.length;
        if (response.synthetic_signature && stats && stats->requests <= 32) {
            std::fputs("NFC READ_SIG: synthetic zero placeholder; no factory ECC signature in BIN\n", stderr);
        }
        if (bytes == 0) {
            if (stats) { ++stats->unsupported; }
            const bool armed = command(0xd1);
            if (stats && stats->requests <= 32) {
                std::fprintf(stderr, "NFC request: cmd=%02x length=%zu response=unsupported\n",
                             opcode, count);
            }
            if (trace) {
                std::printf("Unhandled reader command 0x%02x (%zu bytes)\n", opcode, count);
            }
            return armed;
        }
        const bool sent = send(response);
        read_sent = sent && response.read_access;
        if (read_sent) { progress.queued(response.read_pages); }
        if (stats) {
            if (sent) { ++stats->queued; }
            if (response.valid_bits == 4 && response.data[0] != 0x0a) { ++stats->unsupported; }
            if (stats->requests <= 32) {
                const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - began).count();
                std::fprintf(stderr,
                    "NFC request: cmd=%02x length=%zu response_bytes=%zu bits=%u queued=%d spi_us=%lld\n",
                    opcode, count, bytes, response.valid_bits, int(sent), static_cast<long long>(elapsed));
            }
        }
        return sent;
    }

private:
    bool send(const NtagReply& response)
    {
        std::array<Byte, 541> tx{0x80}, unused{};
        const auto initial = std::min<std::size_t>(512, response.length);
        std::copy_n(response.data.begin(), initial, tx.begin() + 1);
        const auto bits = response.valid_bits ? response.valid_bits : response.length * 8;
        if (!command(0xdb) || !transfer(tx, unused, initial + 1) ||
            !write(0x22, static_cast<Byte>(bits >> 8)) ||
            !write(0x23, static_cast<Byte>(bits)) ||
            !command(response.valid_bits == 4 ? 0xc5 : 0xc4)) { return false; }
        if (response.length > initial) {
            // FIFO is 512 bytes; refill the final 28 bytes of a full NTAG215 read
            // after transmission frees space, without splitting the RF response.
            const auto remaining = response.length - initial;
            const auto deadline = std::chrono::steady_clock::now() + 20ms;
            for (;;) {
                std::array<Byte, 3> fifo{};
                if (!transfer(std::array<Byte, 3>{0x5e}, fifo) || (fifo[2] & 0x30)) { return false; }
                const auto used = static_cast<std::size_t>(fifo[1]) | ((fifo[2] & 0xc0u) << 2);
                if (used <= 512 - remaining) { break; }
                if (std::chrono::steady_clock::now() >= deadline) { return false; }
                std::this_thread::sleep_for(50us);
            }
            std::copy_n(response.data.begin() + initial, remaining, tx.begin() + 1);
            if (!transfer(tx, unused, remaining + 1)) { return false; }
        }
        return true;
    }

    bool write(Byte address, Byte value)
    {
        std::array<Byte, 2> ignored{};
        return transfer(std::array<Byte, 2>{address, value}, ignored);
    }

    bool command(Byte opcode)
    {
        std::array<Byte, 1> ignored{};
        return transfer(std::array<Byte, 1>{opcode}, ignored);
    }

    int fd_ = -1;
    bool configured_ = false;
};

struct ListenState {
    bool selected = false;
    bool active = false;
    bool awaiting_read_tx = false;
    bool read_complete = false;
    ReadProgress progress;
    bool field = false;
};

bool listen_step(Cap& cap, Image& image, NtagSession& protocol, ListenState& state, bool trace,
                 Diagnostics* stats = nullptr)
{
    std::array<Byte, 5> flags{};
    if (!cap.irq(flags)) {
        return false;
    }
    if (stats) {
        if (flags[2] & 0x10) { ++stats->fields; }
        if (flags[4] & 0x03) { ++stats->selections; }
        if (flags[1] & 0x08) { ++stats->sent; }
        if (flags[3] & 0xf0) { ++stats->errors; }
        if (flags[1] || flags[2] || flags[3] || flags[4]) {
            stats->last_irq = (unsigned(flags[1]) << 24) | (unsigned(flags[2]) << 16) |
                              (unsigned(flags[3]) << 8) | flags[4];
        }
    }
    if (trace && (flags[2] & 0x10)) {
        std::puts("External RF field detected");
    }
    if (flags[2] & 0x10) { state.field = true; }
    if (flags[4] & 0x03) {
        state.field = true;
        state.active = true;
        state.selected = true;
        if (!cap.selected()) { return false; }
        if (trace) {
            std::puts("Reader selected the synthetic NFC-A target");
        }
    }
    if (state.active && (flags[1] & 0x08)) {
        if (state.awaiting_read_tx) {
            if (trace) {
                std::puts("NFC-A READ response transmitted");
            }
            state.read_complete = true;
            state.progress.transmitted();
            state.awaiting_read_tx = false;
        }
        if (!cap.rearm()) {
            return false;
        }
    }
    if (flags[2] & 0x08) {
        state.field = false;
        state.active = false;
        state.awaiting_read_tx = false;
        protocol.reset_rf();
        state.progress.end_rf();
        if (trace) {
            std::puts("External RF field ended");
        }
        if (!cap.sense()) {
            return false;
        }
    } else if (state.active && ((flags[1] | flags[4]) & 0x10)) {
        if (flags[3] & 0xf0) {
            if (!cap.discard_frame()) { return false; }
        } else if (!cap.respond(image, protocol, state.awaiting_read_tx, state.progress, state.active, trace, stats)) {
            return false;
        }
    }
    if (trace) {
        std::fflush(stdout);
    }
    if (!state.active) {
        std::this_thread::sleep_for(100us);
    }
    return true;
}

} // namespace

struct CapListener::Session {
    explicit Session(const Image& data) : image(data) {}
    Image image;
    Image source;
    NtagSession protocol;
    AmiiboCrypto* crypto = nullptr;
    Cap cap;
    std::atomic<bool> stop{false};
    std::atomic<bool> failed{false};
    std::atomic<const char*> error{nullptr};
    std::atomic<unsigned> generation{0};
    std::atomic<unsigned> pages{0};
    std::atomic<bool> field{false};
    std::atomic<bool> next{false};
    std::thread worker;
};

CapListener::CapListener(std::filesystem::path directory) : directory_(std::move(directory))
{
    if (!directory_.empty()) { crypto_ = std::make_unique<AmiiboCrypto>(); }
}
CapListener::~CapListener() { stop(); }

const char* CapListener::initialize()
{
    stop();
    if (crypto_) {
        if (const auto* error = crypto_->load(directory_)) { return error; }
    }
    auto session = std::make_unique<Session>(Image{});
    if (!session->cap.open_cap()) {
        return "Cannot access Cap NFC SPI or identify chip.";
    }
    if (!session->cap.initialize()) {
        return "Cap NFC hardware initialization failed.";
    }
    session_ = std::move(session);
    return nullptr;
}

const char* CapListener::start(const Image& image)
{
    if (!session_ || session_->worker.joinable()) {
        return "Initialize NFC before starting a role.";
    }
    auto& session = session_;
    session->source = image;
    session->crypto = crypto_.get();
    if (crypto_) {
        if (const auto* error = crypto_->new_identity(image, session->image)) { return error; }
    } else {
        session->image = image;
    }
    session->protocol = {};
    session->generation = 1;
    session->pages = 0;
    session->field = false;
    session->next = false;
    session->error = nullptr;
    session->stop = false;
    session->failed = false;
    if (!session->cap.prepare(session->image)) {
        return "Cap NFC passive listener setup failed.";
    }
    try {
        session->worker = std::thread([state = session.get()] {
            ListenState listen;
            Diagnostics stats;
            UidCycle cycle;
            std::array<unsigned, 8> previous_report{};
            bool first_report = true;
            auto report_at = std::chrono::steady_clock::now() + 2s;
            while (!state->stop.load()) {
                if (!listen_step(state->cap, state->image, state->protocol, listen, false, &stats)) {
                    state->failed = true;
                    return;
                }
                const auto now = std::chrono::steady_clock::now();
                state->field = listen.field;
                state->pages = listen.progress.pages();
                const bool finished = listen.progress.complete();
                cycle.observe(listen.field, finished || state->next.load(), now);
                if (state->crypto && cycle.ready(now)) {
                    bool present = false;
                    if (!state->cap.field_present(present)) { state->failed = true; return; }
                    if (present) {
                        listen.field = true;
                        cycle.observe(true, false, now);
                    } else {
                        Image next_image;
                        if (!state->cap.quiet()) { state->failed = true; return; }
                        if (const auto* error = state->crypto->new_identity(state->source, next_image)) {
                            state->error = error;
                            state->failed = true;
                            return;
                        }
                        if (!state->cap.prepare(next_image)) { state->failed = true; return; }
                        state->image = next_image;
                        state->protocol = {};
                        listen = {};
                        cycle.rotated();
                        state->pages = 0;
                        state->next = false;
                        const auto generation = ++state->generation;
                        stats = {}; // Keep request diagnostics for every identity.
                        std::fprintf(stderr, "NFC UID cycle: generation=%u authenticated=1\n", generation);
                    }
                }
                if (now >= report_at) {
                    const std::array<unsigned, 8> report{stats.fields, stats.selections, stats.requests,
                        stats.sent, stats.queued, stats.unsupported, stats.errors, state->generation.load()};
                    if (first_report || report != previous_report) {
                        std::fprintf(stderr,
                            "NFC diag: uid_cycle=%u field=%u selected=%u rx=%u tx_irq=%u queued=%u nak=%u errors=%u cmd=%02x irq=%08x pages=%u\n",
                            state->generation.load(), stats.fields, stats.selections, stats.requests, stats.sent,
                            stats.queued, stats.unsupported, stats.errors, stats.last_command, stats.last_irq,
                            state->pages.load());
                        previous_report = report;
                        first_report = false;
                    }
                    report_at = now + 2s;
                }
            }
        });
    } catch (const std::system_error& error) {
        std::fprintf(stderr, "Cannot start Cap NFC worker: %s\n", error.what());
        return "Cannot start NFC worker.";
    }
    return nullptr;
}

void CapListener::stop_session()
{
    if (session_) {
        session_->stop = true;
        if (session_->worker.joinable()) {
            session_->worker.join();
        }
        if (!session_->cap.quiet()) {
            session_->failed = true;
        }
    }
}

void CapListener::stop()
{
    stop_session();
    session_.reset();
}

const char* CapListener::poll_error()
{
    if (!session_ || !session_->failed.load()) { return nullptr; }
    const auto* error = session_->error.load();
    return error ? error : "NFC communication stopped: SPI error.";
}

void CapListener::request_next_identity()
{
    if (session_ && crypto_) { session_->next = true; }
}

const char* CapListener::status()
{
    if (!session_ || !session_->worker.joinable()) { return nullptr; }
    const auto* hint = session_->next.load() ? "Remove 1s for next UID" :
        session_->field.load() ? "Hold still until scan ends" :
        session_->pages.load() == 130 ? "Remove 1s for next UID" : "Ready / retry same UID";
    std::snprintf(status_text_, sizeof(status_text_), "UID #%u | %s\n%u/130 pages | %s",
                  session_->generation.load(), crypto_ ? "Auto UID ON" : "Fixed UID",
                  session_->pages.load(), hint);
    return status_text_;
}

int run_cap_probe()
{
    std::signal(SIGINT, interrupt);
    std::signal(SIGTERM, interrupt);
    Cap cap;
    if (!cap.open_cap() || !cap.initialize() || !cap.prepare(sample)) {
        std::fputs("Passive listener setup failed\n", stderr);
        return 1;
    }
    std::puts("Passive NFC-A probe ready (30 seconds; synthetic UID 04:11:22:33:44:55:66)");
    std::fflush(stdout);
    const auto deadline = std::chrono::steady_clock::now() + 30s;
    ListenState state;
    Image image = sample;
    NtagSession protocol;
    while (!stopped && std::chrono::steady_clock::now() < deadline) {
        if (!listen_step(cap, image, protocol, state, true)) {
            return 1;
        }
    }
    if (!state.selected) {
        std::puts("No reader selection observed");
    }
    return state.read_complete ? 0 : state.selected ? 3 : 2;
}

} // namespace amiibo
