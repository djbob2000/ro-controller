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

Settings fast_settings() {
    Settings s = Settings::defaults();
    s.boot_stabilize_ms = 10;
    s.low_pressure_stable_ms = 10;
    s.low_pressure_restart_delay_ms = 20;
    s.tank_full_debounce_ms = 10;
    s.prepare_ms = 10;
    s.flush_duration_ms = 20;
    s.long_idle_ms = 100;
    s.max_production_ms = 60'000;
    s.service_test_timeout_ms = 30;
    s.standby_flush_interval_s = 3600;
    return s;
}

void reach_producing(Controller& c, uint64_t& now, Inputs in) {
    c.boot(now, {});
    now += 10;
    c.tick(now, in);
    CHECK(c.snapshot().state == State::Prepare);
    now += 10;
    c.tick(now, in);
    CHECK(c.snapshot().state == State::StartupFlush);
    now += 20;
    c.tick(now, in, {true, 1'000, 12 * 60});
    CHECK(c.snapshot().state == State::Producing);
    CHECK(c.snapshot().outputs.inlet);
    CHECK(c.snapshot().outputs.pump);
    CHECK(!c.snapshot().outputs.flush);
}

void test_boot_and_normal_cycle() {
    Controller c{fast_settings()};
    uint64_t now = 0;
    Inputs in{true, false, false};
    c.boot(now, {});
    CHECK(c.snapshot().state == State::Boot);
    CHECK(!c.snapshot().outputs.inlet && !c.snapshot().outputs.pump && !c.snapshot().outputs.flush);

    reach_producing(c, now, in);

    in.tank_full = true;
    now += 1;
    c.tick(now, in);
    CHECK(c.snapshot().state == State::Producing);
    now += 10;
    c.tick(now, in);
    CHECK(c.snapshot().state == State::FinalFlush);
    CHECK(c.snapshot().outputs.inlet && c.snapshot().outputs.pump && c.snapshot().outputs.flush);

    // High-pressure may release as flush opens; final flush must continue.
    in.tank_full = false;
    now += 5;
    c.tick(now, in);
    CHECK(c.snapshot().state == State::FinalFlush);

    now += 15;
    c.tick(now, in, {true, 2'000, 12 * 60});
    CHECK(c.snapshot().state == State::Standby);
    CHECK(c.persistent_facts().last_membrane_flush_utc_s == 2'000);
}

void test_low_pressure_recovery() {
    Controller c{fast_settings()};
    uint64_t now = 0;
    Inputs in{true, false, false};
    reach_producing(c, now, in);

    in.water_available = false;
    ++now;
    c.tick(now, in);
    CHECK(c.snapshot().state == State::WaitWater);
    CHECK(!c.snapshot().outputs.pump);

    in.water_available = true;
    ++now;
    c.tick(now, in);
    CHECK(c.snapshot().state == State::WaitWater);
    now += 29;
    c.tick(now, in);
    CHECK(c.snapshot().state == State::WaitWater);
    ++now;
    c.tick(now, in);
    CHECK(c.snapshot().state == State::Prepare);
}

void test_latched_leak_and_reset() {
    Controller c{fast_settings()};
    uint64_t now = 0;
    Inputs in{true, false, true};
    c.boot(now, {});
    c.tick(now, in);
    CHECK(c.snapshot().state == State::ErrorLeak);
    CHECK(c.persistent_facts().latched_error == ErrorCode::Leak);

    c.command(++now, in, {CommandType::ResetError, CommandSource::Local});
    CHECK(c.snapshot().state == State::ErrorLeak);

    in.leak_detected = false;
    c.command(++now, in, {CommandType::ResetError, CommandSource::Local});
    CHECK(c.snapshot().state == State::Boot);
    CHECK(c.persistent_facts().latched_error == ErrorCode::None);
}

void test_max_runtime_latch() {
    Controller c{fast_settings()};
    uint64_t now = 0;
    Inputs in{true, false, false};
    reach_producing(c, now, in);
    now += 60'000;
    c.tick(now, in);
    CHECK(c.snapshot().state == State::ErrorMaxRuntime);
    CHECK(c.persistent_facts().latched_error == ErrorCode::MaxRuntime);
    CHECK(!c.snapshot().outputs.pump);
}

void test_maintenance_is_local_only() {
    Controller c{fast_settings()};
    Inputs in{true, true, false};
    c.boot(0, {});
    c.tick(10, in);
    CHECK(c.snapshot().state == State::Standby);

    c.command(11, in, {CommandType::EnterMaintenance, CommandSource::Web});
    CHECK(c.snapshot().state == State::Standby);
    c.command(12, in, {CommandType::EnterMaintenance, CommandSource::Local});
    CHECK(c.snapshot().state == State::Maintenance);

    c.command(13, in, {CommandType::TestPump, CommandSource::Mqtt});
    CHECK(c.snapshot().service_test == ServiceTest::None);
    c.command(14, in, {CommandType::TestPump, CommandSource::Local});
    CHECK(c.snapshot().service_test == ServiceTest::Pump);
    CHECK(c.snapshot().outputs.pump);

    c.tick(44, in);
    CHECK(c.snapshot().service_test == ServiceTest::None);
    CHECK(!c.snapshot().outputs.pump);
}

void test_ota_hold_requires_internal_source() {
    Controller c{fast_settings()};
    Inputs in{true, true, false};
    c.boot(0, {});
    c.tick(10, in);
    CHECK(c.snapshot().state == State::Standby);

    c.command(11, in, {CommandType::EnterOtaHold, CommandSource::Web});
    CHECK(c.snapshot().state == State::Standby);
    c.command(12, in, {CommandType::EnterOtaHold, CommandSource::Internal});
    CHECK(c.snapshot().state == State::OtaHold);
    CHECK(!c.snapshot().outputs.pump && !c.snapshot().outputs.inlet && !c.snapshot().outputs.flush);
}

void test_quiet_hours_prefush() {
    Settings s = fast_settings();
    Controller c{s};
    PersistentFacts facts{};
    facts.last_membrane_flush_utc_s = 10'000;
    Inputs in{true, true, false};
    c.boot(0, facts);
    c.tick(10, in, {true, 10'100, static_cast<uint16_t>(21 * 60 + 59)});
    CHECK(c.snapshot().state == State::Standby);

    // Due time is within the upcoming quiet window, so flush is pulled to 22:00.
    c.tick(11, in, {true, 10'200, static_cast<uint16_t>(22 * 60)});
    CHECK(c.snapshot().state == State::StandbyFlush);
}

} // namespace

int main() {
    CHECK(validate(fast_settings()).ok);
    test_boot_and_normal_cycle();
    test_low_pressure_recovery();
    test_latched_leak_and_reset();
    test_max_runtime_latch();
    test_maintenance_is_local_only();
    test_ota_hold_requires_internal_source();
    test_quiet_hours_prefush();
    std::cout << "All RO controller host tests passed\n";
    return 0;
}
