#include "cap_listener.h"

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

    bool prepare(const Image& image)
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

    bool sense() { return command(0xcd); }
    bool rearm() { return command(0xd1); }

    bool respond(const Image& image, bool& read_sent, bool trace)
    {
        std::array<Byte, 3> fifo{};
        if (!transfer(std::array<Byte, 3>{0x5e}, fifo)) {
            return false;
        }
        const auto count = static_cast<std::size_t>(fifo[1]) | ((fifo[2] & 0xc0u) << 2);
        if (count == 0) {
            return true;
        }
        std::array<Byte, 8> request{};
        if (count >= request.size()) {
            return command(0xdb) && command(0xd1);
        }
        if (!transfer(std::array<Byte, 8>{0x9f}, request, count + 1)) {
            return false;
        }
        std::array<Byte, 64> reply{0x80};
        std::size_t bytes = 0;
        const auto page = request[2];
        switch (request[1]) {
        case 0x30: // READ
            if ((count == 2 || count == 4) && page < image.size() / 4) {
                for (std::size_t i = 0; i < 16; ++i) {
                    reply[i + 1] = image[(4 * page + i) % image.size()];
                }
                bytes = 16;
            }
            break;
        case 0x3a: // FAST_READ
            // Bound the single-FIFO response to 15 pages until larger frames are verified.
            if ((count == 3 || count == 5) && request[3] < image.size() / 4 &&
                page <= request[3] && request[3] - page < 15) {
                bytes = 4 * (request[3] - page + 1);
                std::copy_n(image.begin() + 4 * page, bytes, reply.begin() + 1);
            }
            break;
        case 0x60: { // GET_VERSION
            const std::array<Byte, 8> version{0, 0x04, 0x04, 0x02, 0x01, 0, 0x11, 0x03};
            if (count == 1 || count == 3) {
                std::copy(version.begin(), version.end(), reply.begin() + 1);
                bytes = version.size();
            }
            break;
        }
        }
        if (bytes == 0) {
            const bool armed = command(0xd1);
            if (trace) {
                std::printf("Unhandled reader command 0x%02x (%zu bytes)\n", request[1], count);
            }
            return armed;
        }
        std::array<Byte, 64> unused{};
        const bool sent = command(0xdb) && transfer(reply, unused, bytes + 1) &&
                          write(0x22, static_cast<Byte>((bytes * 8) >> 8)) &&
                          write(0x23, static_cast<Byte>(bytes * 8)) &&
                          command(0xc4);
        read_sent = sent && request[1] == 0x30;
        return sent;
    }

private:
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
};

bool listen_step(Cap& cap, const Image& image, ListenState& state, bool trace)
{
    std::array<Byte, 5> flags{};
    if (!cap.irq(flags)) {
        return false;
    }
    if (trace && (flags[2] & 0x10)) {
        std::puts("External RF field detected");
    }
    if (flags[4] & 0x03) {
        state.active = true;
        state.selected = true;
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
            state.awaiting_read_tx = false;
        }
        if (!cap.rearm()) {
            return false;
        }
    }
    if (flags[2] & 0x08) {
        state.active = false;
        state.awaiting_read_tx = false;
        if (trace) {
            std::puts("External RF field ended");
        }
        if (!cap.sense()) {
            return false;
        }
    } else if (state.active && ((flags[1] | flags[4]) & 0x10) &&
               !(flags[3] & 0xf0) && !cap.respond(image, state.awaiting_read_tx, trace)) {
        return false;
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
    Cap cap;
    std::atomic<bool> stop{false};
    std::atomic<bool> failed{false};
    std::thread worker;
};

CapListener::CapListener() = default;
CapListener::~CapListener() { stop(); }

const char* CapListener::start(const Image& image)
{
    stop();
    auto session = std::make_unique<Session>(image);
    if (!session->cap.open_cap()) {
        return "Cannot access Cap NFC SPI or identify chip.";
    }
    if (!session->cap.prepare(session->image)) {
        return "Cap NFC passive listener setup failed.";
    }
    try {
        session->worker = std::thread([state = session.get()] {
            ListenState listen;
            while (!state->stop.load()) {
                if (!listen_step(state->cap, state->image, listen, false)) {
                    state->failed = true;
                    return;
                }
            }
        });
    } catch (const std::system_error& error) {
        std::fprintf(stderr, "Cannot start Cap NFC worker: %s\n", error.what());
        return "Cannot start NFC worker.";
    }
    session_ = std::move(session);
    return nullptr;
}

void CapListener::stop()
{
    if (session_) {
        session_->stop = true;
        session_->worker.join();
        session_.reset();
    }
}

const char* CapListener::poll_error()
{
    return session_ && session_->failed.load() ? "NFC communication stopped: SPI error." : nullptr;
}

int run_cap_probe()
{
    std::signal(SIGINT, interrupt);
    std::signal(SIGTERM, interrupt);
    Cap cap;
    if (!cap.open_cap() || !cap.prepare(sample)) {
        std::fputs("Passive listener setup failed\n", stderr);
        return 1;
    }
    std::puts("Passive NFC-A probe ready (30 seconds; synthetic UID 04:11:22:33:44:55:66)");
    std::fflush(stdout);
    const auto deadline = std::chrono::steady_clock::now() + 30s;
    ListenState state;
    while (!stopped && std::chrono::steady_clock::now() < deadline) {
        if (!listen_step(cap, sample, state, true)) {
            return 1;
        }
    }
    if (!state.selected) {
        std::puts("No reader selection observed");
    }
    return state.read_complete ? 0 : state.selected ? 3 : 2;
}

} // namespace amiibo
