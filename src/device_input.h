#pragma once

#include <chrono>
#include <optional>

class DeviceInput {
public:
    using Clock = std::chrono::steady_clock;

    void key(char key, bool pressed, Clock::time_point now)
    {
        if (key == '\x1b') {
            if (pressed) {
                if (!esc_start_) {
                    esc_start_ = now;
                }
            } else {
                tick(now);
                cancel_hold();
            }
        } else if (pressed) {
            switch (key) {
            case 'z': column_ = 0; break;
            case 'x': row_ = 1; break;
            case 'c': column_ = 1; break;
            case 'f': row_ = 0; break;
            }
        }
    }

    void tick(Clock::time_point now)
    {
        if (esc_start_ && now - *esc_start_ >= std::chrono::seconds(3)) {
            quit_ = true;
        }
    }

    void cancel_hold() { esc_start_.reset(); }
    bool holding() const { return esc_start_.has_value(); }
    bool quitting() const { return quit_; }
    int focused_tile() const { return row_ * 2 + column_; }

private:
    std::optional<Clock::time_point> esc_start_;
    bool quit_ = false;
    int column_ = 0;
    int row_ = 0;
};
