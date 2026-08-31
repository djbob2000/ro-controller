#include "ro/controller.hpp"

#include <algorithm>

namespace ro {

ValidationResult validate(const Settings& s) noexcept {
    if (s.boot_stabilize_ms == 0) return {false, "boot_stabilize_ms"};
    if (s.low_pressure_stable_ms == 0) return {false, "low_pressure_stable_ms"};
    if (s.tank_full_debounce_ms == 0) return {false, "tank_full_debounce_ms"};
    if (s.prepare_ms == 0) return {false, "prepare_ms"};
    if (s.flush_duration_ms == 0 || s.flush_duration_ms > 10ULL * 60 * 1000) return {false, "flush_duration_ms"};
    if (s.max_production_ms < 60'000) return {false, "max_production_ms"};
    if (s.service_test_timeout_ms == 0 || s.service_test_timeout_ms > 120'000) return {false, "service_test_timeout_ms"};
    if (s.quiet_start_minutes > 1439) return {false, "quiet_start_minutes"};
    if (s.quiet_end_minutes > 1439) return {false, "quiet_end_minutes"};
    if (s.standby_flush_interval_s < 3600) return {false, "standby_flush_interval_s"};
    return {true, nullptr};
}

Controller::Controller(Settings settings) noexcept : settings_(settings) {}

void Controller::set_settings(const Settings& settings) noexcept {
    if (validate(settings).ok) settings_ = settings;
}

void Controller::set_persistent_facts(const PersistentFacts& facts) noexcept {
    facts_ = facts;
    snapshot_.error = facts_.latched_error;
}

void Controller::boot(uint64_t now_ms, const PersistentFacts& facts) noexcept {
    facts_ = facts;
    snapshot_ = {};
    snapshot_.mode = Mode::Auto;
    snapshot_.state = State::Boot;
    snapshot_.error = facts_.latched_error;
    state_entered_ms_ = now_ms;
    production_started_ms_ = 0;
    water_stability_tracking_ = false;
    tank_debounce_tracking_ = false;
    startup_flush_required_ = true;
    quiet_prefush_consumed_for_window_ = false;
    service_deadline_ms_ = 0;
    manual_flush_return_state_ = State::Standby;
    apply_outputs(now_ms);
}

bool Controller::is_error_state() const noexcept {
    return snapshot_.state == State::ErrorLeak || snapshot_.state == State::ErrorMaxRuntime;
}

void Controller::transition(State next, uint64_t now_ms, ControllerEvents& ev) noexcept {
    if (snapshot_.state == next) return;
    ev.state_changed = true;
    ev.previous_state = snapshot_.state;
    ev.new_state = next;
    snapshot_.state = next;
    state_entered_ms_ = now_ms;
    if (next == State::Standby) standby_entered_ms_ = now_ms;
    if (next == State::Producing) {
        production_started_ms_ = now_ms;
        ev.production_started = true;
    }
    if (next != State::Producing) tank_debounce_tracking_ = false;
}

void Controller::latch_error(ErrorCode error, uint64_t now_ms, ControllerEvents& ev) noexcept {
    if (error == ErrorCode::None) return;
    facts_.latched_error = error;
    snapshot_.error = error;
    ev.error_latched = true;
    ev.error = error;
    transition(error == ErrorCode::Leak ? State::ErrorLeak : State::ErrorMaxRuntime, now_ms, ev);
}

bool Controller::clear_error_if_allowed(const Inputs& inputs, uint64_t now_ms, ControllerEvents& ev) noexcept {
    if (facts_.latched_error == ErrorCode::None) return true;
    if (facts_.latched_error == ErrorCode::Leak && inputs.leak_detected) return false;
    facts_.latched_error = ErrorCode::None;
    snapshot_.error = ErrorCode::None;
    ev.error_cleared = true;
    ev.error = ErrorCode::None;
    transition(State::Boot, now_ms, ev);
    snapshot_.mode = Mode::Auto;
    startup_flush_required_ = true;
    return true;
}

bool Controller::is_quiet(uint16_t minute) const noexcept {
    if (!settings_.quiet_hours_enabled) return false;
    const auto start = settings_.quiet_start_minutes;
    const auto end = settings_.quiet_end_minutes;
    if (start == end) return false;
    if (start < end) return minute >= start && minute < end;
    return minute >= start || minute < end;
}

bool Controller::preventive_flush_due(TimeInfo time) noexcept {
    if (!settings_.standby_flush_enabled || !time.valid || !facts_.last_membrane_flush_utc_s) return false;
    const int64_t due = *facts_.last_membrane_flush_utc_s + static_cast<int64_t>(settings_.standby_flush_interval_s);
    const bool overdue = time.utc_epoch_s >= due;
    if (!settings_.quiet_hours_enabled) return overdue;

    if (time.local_minute == settings_.quiet_start_minutes && !quiet_prefush_consumed_for_window_) {
        uint32_t quiet_minutes = 0;
        const auto start = settings_.quiet_start_minutes;
        const auto end = settings_.quiet_end_minutes;
        if (start < end) quiet_minutes = end - start;
        else quiet_minutes = (24U * 60U - start) + end;
        const int64_t quiet_end_epoch = time.utc_epoch_s + static_cast<int64_t>(quiet_minutes) * 60;
        if (due <= quiet_end_epoch) {
            quiet_prefush_consumed_for_window_ = true;
            return true;
        }
    }
    if (overdue) {
        if (is_quiet(time.local_minute)) return false;
        quiet_prefush_consumed_for_window_ = false;
        return true;
    }
    if (time.local_minute != settings_.quiet_start_minutes && !is_quiet(time.local_minute)) {
        quiet_prefush_consumed_for_window_ = false;
    }
    return false;
}

void Controller::complete_membrane_flush(TimeInfo time, ControllerEvents& ev) noexcept {
    ev.membrane_flush_completed = true;
    if (time.valid) facts_.last_membrane_flush_utc_s = time.utc_epoch_s;
}

void Controller::apply_outputs(uint64_t now_ms) noexcept {
    (void)now_ms;
    DesiredOutputs out{};
    switch (snapshot_.state) {
        case State::Prepare: out.inlet = true; break;
        case State::StartupFlush:
        case State::FinalFlush:
        case State::StandbyFlush:
            out = {true, true, true}; break;
        case State::Producing:
            out = {true, true, false}; break;
        case State::Maintenance:
            switch (snapshot_.service_test) {
                case ServiceTest::Inlet: out.inlet = true; break;
                case ServiceTest::Pump: out = {true, true, false}; break;
                case ServiceTest::Flush: out.flush = true; break;
                case ServiceTest::ManualFlush: out = {true, true, true}; break;
                case ServiceTest::None: break;
            }
            break;
        default: break;
    }
    if (is_error_state() || snapshot_.ota_hold) out = {};
    snapshot_.outputs = out;
}

ControllerEvents Controller::tick(uint64_t now_ms, const Inputs& inputs, TimeInfo time) noexcept {
    ControllerEvents ev{};
    snapshot_.inputs = inputs;
    snapshot_.state_age_ms = now_ms - state_entered_ms_;
    snapshot_.production_runtime_ms = snapshot_.state == State::Producing ? now_ms - production_started_ms_ : 0;

    if (facts_.latched_error != ErrorCode::None && !is_error_state()) {
        snapshot_.error = facts_.latched_error;
        transition(facts_.latched_error == ErrorCode::Leak ? State::ErrorLeak : State::ErrorMaxRuntime, now_ms, ev);
    }
    if (inputs.leak_detected && facts_.latched_error != ErrorCode::Leak) {
        latch_error(ErrorCode::Leak, now_ms, ev);
    }
    if (is_error_state() || snapshot_.state == State::OtaHold) {
        apply_outputs(now_ms);
        return ev;
    }

    switch (snapshot_.state) {
        case State::Boot:
            if (now_ms - state_entered_ms_ >= settings_.boot_stabilize_ms) {
                if (!inputs.water_available) transition(State::WaitWater, now_ms, ev);
                else if (inputs.tank_full) transition(State::Standby, now_ms, ev);
                else {
                    startup_flush_required_ = true;
                    transition(State::Prepare, now_ms, ev);
                }
            }
            break;

        case State::WaitWater:
            if (!inputs.water_available) {
                water_stability_tracking_ = false;
            } else {
                if (!water_stability_tracking_) {
                    water_stability_tracking_ = true;
                    water_stable_since_ms_ = now_ms;
                }
                const uint64_t wait_ms = settings_.low_pressure_stable_ms + settings_.low_pressure_restart_delay_ms;
                if (now_ms - water_stable_since_ms_ >= wait_ms) {
                    water_stability_tracking_ = false;
                    if (inputs.tank_full) transition(State::Standby, now_ms, ev);
                    else {
                        startup_flush_required_ = true;
                        transition(State::Prepare, now_ms, ev);
                    }
                }
            }
            break;

        case State::Prepare:
            if (!inputs.water_available) transition(State::WaitWater, now_ms, ev);
            else if (inputs.tank_full) transition(State::Standby, now_ms, ev);
            else if (now_ms - state_entered_ms_ >= settings_.prepare_ms) {
                transition(startup_flush_required_ ? State::StartupFlush : State::Producing, now_ms, ev);
                startup_flush_required_ = false;
            }
            break;

        case State::StartupFlush:
            if (!inputs.water_available) transition(State::WaitWater, now_ms, ev);
            else if (now_ms - state_entered_ms_ >= settings_.flush_duration_ms) {
                complete_membrane_flush(time, ev);
                transition(State::Producing, now_ms, ev);
            }
            break;

        case State::Producing:
            if (!inputs.water_available) {
                transition(State::WaitWater, now_ms, ev);
                break;
            }
            if (now_ms - production_started_ms_ >= settings_.max_production_ms) {
                latch_error(ErrorCode::MaxRuntime, now_ms, ev);
                break;
            }
            if (inputs.tank_full) {
                if (!tank_debounce_tracking_) {
                    tank_debounce_tracking_ = true;
                    tank_full_since_ms_ = now_ms;
                }
                if (now_ms - tank_full_since_ms_ >= settings_.tank_full_debounce_ms) {
                    ev.tank_became_full = true;
                    transition(State::FinalFlush, now_ms, ev);
                }
            } else {
                tank_debounce_tracking_ = false;
            }
            break;

        case State::FinalFlush:
            if (!inputs.water_available) transition(State::WaitWater, now_ms, ev);
            else if (now_ms - state_entered_ms_ >= settings_.flush_duration_ms) {
                complete_membrane_flush(time, ev);
                transition(State::Standby, now_ms, ev);
            }
            break;

        case State::Standby:
            if (!inputs.water_available) {
                transition(State::WaitWater, now_ms, ev);
                break;
            }
            if (!inputs.tank_full) {
                startup_flush_required_ = now_ms - standby_entered_ms_ >= settings_.long_idle_ms;
                transition(State::Prepare, now_ms, ev);
                break;
            }
            if (preventive_flush_due(time)) {
                manual_flush_return_state_ = State::Standby;
                transition(State::StandbyFlush, now_ms, ev);
            }
            break;

        case State::StandbyFlush:
            if (!inputs.water_available) transition(State::WaitWater, now_ms, ev);
            else if (now_ms - state_entered_ms_ >= settings_.flush_duration_ms) {
                complete_membrane_flush(time, ev);
                transition(manual_flush_return_state_, now_ms, ev);
            }
            break;

        case State::Maintenance:
            if (snapshot_.service_test != ServiceTest::None && now_ms >= service_deadline_ms_) {
                snapshot_.service_test = ServiceTest::None;
            }
            if (snapshot_.service_test == ServiceTest::Pump && !inputs.water_available) {
                snapshot_.service_test = ServiceTest::None;
            }
            if (snapshot_.service_test == ServiceTest::ManualFlush && !inputs.water_available) {
                snapshot_.service_test = ServiceTest::None;
            }
            break;

        default:
            break;
    }

    snapshot_.state_age_ms = now_ms - state_entered_ms_;
    snapshot_.production_runtime_ms = snapshot_.state == State::Producing ? now_ms - production_started_ms_ : 0;
    apply_outputs(now_ms);
    return ev;
}

ControllerEvents Controller::command(uint64_t now_ms, const Inputs& inputs, const Command& cmd, TimeInfo time) noexcept {
    ControllerEvents ev{};
    snapshot_.inputs = inputs;

    if (cmd.type == CommandType::ResetError) {
        clear_error_if_allowed(inputs, now_ms, ev);
        apply_outputs(now_ms);
        return ev;
    }
    if (is_error_state()) return ev;

    if (cmd.type == CommandType::EnterOtaHold && cmd.source == CommandSource::Internal) {
        snapshot_.ota_hold = true;
        transition(State::OtaHold, now_ms, ev);
        apply_outputs(now_ms);
        return ev;
    }
    if (cmd.type == CommandType::ExitOtaHold && cmd.source == CommandSource::Internal && snapshot_.state == State::OtaHold) {
        snapshot_.ota_hold = false;
        transition(State::Boot, now_ms, ev);
        startup_flush_required_ = true;
        apply_outputs(now_ms);
        return ev;
    }

    if (cmd.type == CommandType::EnterMaintenance) {
        if (cmd.source == CommandSource::Local && snapshot_.state != State::Maintenance) {
            snapshot_.mode = Mode::Maintenance;
            snapshot_.service_test = ServiceTest::None;
            transition(State::Maintenance, now_ms, ev);
        }
        apply_outputs(now_ms);
        return ev;
    }
    if (cmd.type == CommandType::ExitMaintenance) {
        if (cmd.source == CommandSource::Local && snapshot_.state == State::Maintenance) {
            snapshot_.mode = Mode::Auto;
            snapshot_.service_test = ServiceTest::None;
            transition(State::Boot, now_ms, ev);
            startup_flush_required_ = true;
        }
        apply_outputs(now_ms);
        return ev;
    }

    if (snapshot_.state == State::Maintenance) {
        if (cmd.source != CommandSource::Local) return ev;
        switch (cmd.type) {
            case CommandType::TestInlet:
                snapshot_.service_test = ServiceTest::Inlet;
                service_deadline_ms_ = now_ms + settings_.service_test_timeout_ms;
                break;
            case CommandType::TestPump:
                if (inputs.water_available) {
                    snapshot_.service_test = ServiceTest::Pump;
                    service_deadline_ms_ = now_ms + settings_.service_test_timeout_ms;
                }
                break;
            case CommandType::TestFlush:
                snapshot_.service_test = ServiceTest::Flush;
                service_deadline_ms_ = now_ms + settings_.service_test_timeout_ms;
                break;
            case CommandType::StartManualFlush:
                if (inputs.water_available) {
                    snapshot_.service_test = ServiceTest::ManualFlush;
                    service_deadline_ms_ = now_ms + settings_.flush_duration_ms;
                }
                break;
            case CommandType::CancelTest:
                snapshot_.service_test = ServiceTest::None;
                break;
            default: break;
        }
        apply_outputs(now_ms);
        return ev;
    }

    if (cmd.type == CommandType::StartManualFlush && inputs.water_available && snapshot_.mode == Mode::Auto) {
        if (snapshot_.state == State::Standby || snapshot_.state == State::Producing) {
            manual_flush_return_state_ = snapshot_.state == State::Producing ? State::Producing : State::Standby;
            transition(State::StandbyFlush, now_ms, ev);
        }
    }

    apply_outputs(now_ms);
    (void)time;
    return ev;
}

const char* to_string(State state) noexcept {
    switch (state) {
        case State::Boot: return "BOOT";
        case State::WaitWater: return "WAIT_WATER";
        case State::Prepare: return "PREPARE";
        case State::StartupFlush: return "STARTUP_FLUSH";
        case State::Producing: return "PRODUCING";
        case State::FinalFlush: return "FINAL_FLUSH";
        case State::Standby: return "STANDBY";
        case State::StandbyFlush: return "STANDBY_FLUSH";
        case State::Maintenance: return "MAINTENANCE";
        case State::OtaHold: return "OTA_HOLD";
        case State::ErrorLeak: return "ERROR_LEAK";
        case State::ErrorMaxRuntime: return "ERROR_MAX_RUNTIME";
    }
    return "UNKNOWN";
}

const char* to_string(ErrorCode error) noexcept {
    switch (error) {
        case ErrorCode::None: return "NONE";
        case ErrorCode::Leak: return "LEAK";
        case ErrorCode::MaxRuntime: return "MAX_RUNTIME";
    }
    return "UNKNOWN";
}

} // namespace ro
