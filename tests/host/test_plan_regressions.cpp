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
    s.boot_stabilize_ms = 10;
    s.low_pressure_stable_ms = 10;
    s.low_pressure_restart_delay_ms = 10;
    s.tank_full_debounce_ms = 10;
    s.prepare_ms = 10;
    s.flush_duration_ms = 20;
    s.long_idle_ms = 100;
    s.max_production_ms = 60'000;
    s.service_test_timeout_ms = 30;
    s.standby_flush_interval_s = 3600;
    s.quiet_hours_enabled = true;
    s.quiet_start_minutes = 22 * 60;
    s.quiet_end_minutes = 8 * 60;
    return s;
}

void boot_to_standby(Controller& c, uint64_t now, const PersistentFacts& facts = {}) {
    c.boot(now, facts);
    c.tick(now + 10, {true, true, false});
    CHECK(c.snapshot().state == State::Standby);
}

void test_latch_survives_reboot_and_authorized_reset() {
    PersistentFacts facts{};
    facts.latched_error = ErrorCode::MaxRuntime;
    Controller rebooted{settings()};
    rebooted.boot(0, facts);
    rebooted.tick(1, {true, false, false});
    CHECK(rebooted.snapshot().state == State::ErrorMaxRuntime);
    CHECK(!rebooted.snapshot().outputs.inlet);
    CHECK(!rebooted.snapshot().outputs.pump);
    CHECK(!rebooted.snapshot().outputs.flush);

    const auto ev = rebooted.command(2, {true, false, false}, {CommandType::ResetError, CommandSource::Web});
    CHECK(ev.error_cleared);
    CHECK(rebooted.persistent_facts().latched_error == ErrorCode::None);
    CHECK(rebooted.snapshot().state == State::Boot);
}

void test_overdue_quiet_hours_waits_until_end() {
    Controller c{settings()};
    PersistentFacts facts{};
    facts.last_membrane_flush_utc_s = 1'000;
    boot_to_standby(c, 0, facts);

    // 03:00: already overdue, but preventive flush is suppressed during quiet hours.
    c.tick(20, {true, true, false}, {true, 5'000, static_cast<uint16_t>(3 * 60)});
    CHECK(c.snapshot().state == State::Standby);

    // At quiet-hours end the overdue flush may run.
    c.tick(30, {true, true, false}, {true, 5'001, static_cast<uint16_t>(8 * 60)});
    CHECK(c.snapshot().state == State::StandbyFlush);
}

void test_unknown_time_never_runs_preventive_flush() {
    Controller c{settings()};
    PersistentFacts facts{};
    facts.last_membrane_flush_utc_s = 1;
    boot_to_standby(c, 0, facts);
    for (uint64_t now = 20; now < 500; now += 20) c.tick(now, {true, true, false}, {});
    CHECK(c.snapshot().state == State::Standby);
}

void test_uninterrupted_short_idle_skips_startup_flush() {
    Controller c{settings()};
    boot_to_standby(c, 0);
    c.tick(99, {true, false, false});
    CHECK(c.snapshot().state == State::Prepare);
    c.tick(109, {true, false, false});
    CHECK(c.snapshot().state == State::Producing);
}

void test_long_idle_requires_startup_flush() {
    Controller c{settings()};
    boot_to_standby(c, 0);
    c.tick(110, {true, false, false});
    CHECK(c.snapshot().state == State::Prepare);
    c.tick(120, {true, false, false});
    CHECK(c.snapshot().state == State::StartupFlush);
}

void test_cold_boot_with_demand_always_flushes() {
    Controller c{settings()};
    c.boot(0, {});
    c.tick(10, {true, false, false});
    CHECK(c.snapshot().state == State::Prepare);
    c.tick(20, {true, false, false});
    CHECK(c.snapshot().state == State::StartupFlush);
}
}

int main() {
    test_latch_survives_reboot_and_authorized_reset();
    test_overdue_quiet_hours_waits_until_end();
    test_unknown_time_never_runs_preventive_flush();
    test_uninterrupted_short_idle_skips_startup_flush();
    test_long_idle_requires_startup_flush();
    test_cold_boot_with_demand_always_flushes();
    std::cout << "Plan regression tests passed\n";
    return 0;
}
