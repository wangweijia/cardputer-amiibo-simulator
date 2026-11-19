#pragma once

#include <chrono>
#include <bitset>

namespace amiibo {

// Count only pages whose response reached the chip's TX-complete interrupt.
// An interrupted RF session must not accumulate fragments from separate scans.
class ReadProgress {
public:
    void queued(const std::bitset<130>& pages) { pending_ = pages; }
    void transmitted() { sent_ |= pending_; pending_.reset(); }
    void end_rf() { pending_.reset(); if (!complete()) { sent_.reset(); } }
    bool complete() const { return sent_.all() && pending_.none(); }
    unsigned pages() const { return static_cast<unsigned>(sent_.count()); }
private:
    std::bitset<130> sent_;
    std::bitset<130> pending_;
};

// A new identity is allowed only between RF sessions, after a stable removal.
class UidCycle {
public:
    using Clock = std::chrono::steady_clock;
    void observe(bool field, bool complete, Clock::time_point now)
    {
        eligible_ = eligible_ || complete;
        if (field || field_ || !observed_) { absent_since_ = now; }
        field_ = field;
        observed_ = true;
    }
    bool ready(Clock::time_point now) const
    {
        return observed_ && eligible_ && !field_ && now - absent_since_ >= std::chrono::seconds(1);
    }
    void rotated() { eligible_ = false; observed_ = false; }
private:
    Clock::time_point absent_since_{};
    bool field_ = false;
    bool eligible_ = false;
    bool observed_ = false;
};

} // namespace amiibo
