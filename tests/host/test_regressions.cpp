#include "ro/controller.hpp"

#include <cstdlib>
#include <iostream>

using namespace ro;

namespace {
[[noreturn]] void fail(const char* expr, int line) {
    std::cerr << "FAIL line " << line << ": " << expr << '\n';
    std::exit(1);
}
#define CHECK(expr) do { if (!(expr)) fail(#expr, __LINE__); } while (0)

Settings settings() {
    Settings s = Settings::defaults();
    s.boot_stabilize_ms = 1;
    s.prepare_ms = 1;
    s.flush_duration_ms = 20;
    s.tank_full_debounce_ms = 1;
    s.low_pressure_stable_ms = 1;
    s.low_pressure_restart_delay_ms = 1;
    s.max_production_ms = 60'000;
    s.standby_flush_interval_s = 3600;
    return s;
}

void reach_producing(Controller& c, uint64_t& now) {
    const Inputs in{true, false, false};
    c.boot(now, {});
    c.tick(++now, in, {true, 1'000, 12 * 60});
    CHECK(c.snapshot().state == State::Prepare);
    c.tick(++now, in, {true, 1'001, 12 * 60});
    CHECK(c.snapshot().state == State::StartupFlush);
    now += 20;
    c.tick(now, in, {true, 1'021, 12 * 60});
    CHECK(c.snapshot().state == State::Producing);
}

void manual_flush_must_not_reset_max_runtime_clock() {
    Controller c{settings()};
    uint64_t now = 0;
    const Inputs in{true, false, false};
    reach_producing(c, now);
    const uint64_t production_started = now;

    now = production_started + 59'990;
    c.tick(now, in, {true, 2'000, 12 * 60});
    CHECK(c.snapshot().state == State::Producing);
    c.command(now, in, {CommandType::StartManualFlush, CommandSource::Web});
    CHECK(c.snapshot().state == State::StandbyFlush);

    now += 20;
    c.tick(now, in, {true, 2'020, 12 * 60});
    CHECK(c.snapshot().state == State::Producing);

    // The production safety budget belongs to the same fill cycle. A manual
    // flush must not allow a remote client to reset the 3h safety timer.
    c.tick(now + 1, in, {true, 2'021, 12 * 60});
    CHECK(c.snapshot().state == State::ErrorMaxRuntime);
}

void first_standby_with_valid_clock_must_anchor_preventive_scheduler() {
    Controller c{settings()};
    uint64_t now = 0;
    const Inputs full{true, true, false};
    c.boot(now, {});
    c.tick(++now, full, {true, 10'000, 12 * 60});
    CHECK(c.snapshot().state == State::Standby);

    // Fresh installations have no persisted previous flush. Anchor the 24h
    // schedule to the first trustworthy clock reading instead of disabling
    // preventive flush forever.
    c.tick(++now, full, {true, 10'001, 12 * 60});
    CHECK(c.persistent_facts().last_membrane_flush_utc_s.has_value());
    CHECK(*c.persistent_facts().last_membrane_flush_utc_s == 10'001);
}
}

int main() {
    manual_flush_must_not_reset_max_runtime_clock();
    first_standby_with_valid_clock_must_anchor_preventive_scheduler();
    std::cout << "RO controller regression tests passed\n";
    return 0;
}
