#include "uid_cycle.h"
#include <cstdlib>
#include <iostream>

void check(bool ok) { if (!ok) { std::cerr << "Unsafe UID rotation timing\n"; std::exit(1); } }
int main()
{
    using namespace std::chrono_literals;
    const auto zero = amiibo::UidCycle::Clock::time_point{};
    amiibo::ReadProgress progress;
    std::bitset<130> first, tail;
    for (unsigned i = 0; i < 120; ++i) { first.set(i); }
    for (unsigned i = 120; i < 130; ++i) { tail.set(i); }
    progress.queued(first);
    check(progress.pages() == 0 && !progress.complete());
    progress.transmitted();
    check(progress.pages() == 120 && !progress.complete());
    progress.queued(tail);
    check(!progress.complete()); // Enqueuing the final response does not prove transmission.
    progress.end_rf();
    check(progress.pages() == 0 && !progress.complete());
    progress.queued(tail);
    progress.transmitted();
    check(!progress.complete()); // A retry cannot reuse a previous session's partial read.
    progress.queued(first);
    progress.transmitted();
    check(progress.pages() == 130 && progress.complete());
    progress.end_rf();
    check(progress.complete()); // Complete scan survives removal until identity changes.
    amiibo::UidCycle cycle;
    check(!cycle.ready(zero + 10s));
    cycle.observe(false, false, zero);
    check(!cycle.ready(zero + 10s));
    cycle.observe(true, true, zero + 10s);
    check(!cycle.ready(zero + 20s)); // Data read is never sufficient while RF is present.
    cycle.observe(true, false, zero + 20s);
    cycle.observe(false, false, zero + 20s + 1ms);
    check(!cycle.ready(zero + 20s + 999ms));
    check(cycle.ready(zero + 21s + 1ms));
    cycle.rotated();
    check(!cycle.ready(zero + 50s));
    cycle.observe(false, false, zero + 50s);
    check(!cycle.ready(zero + 60s)); // An idle tag must not rotate repeatedly.
    cycle.observe(false, true, zero + 60s); // Manual request when already outside RF.
    check(cycle.ready(zero + 60s));
    cycle.rotated();
    cycle.observe(true, true, zero + 70s);
    cycle.observe(false, false, zero + 70s + 100ms);
    cycle.observe(true, false, zero + 70s + 900ms); // Brief field gaps must not change identity.
    check(!cycle.ready(zero + 72s));
    cycle.observe(false, false, zero + 72s);
    check(!cycle.ready(zero + 72s));
    check(cycle.ready(zero + 73s));
}
