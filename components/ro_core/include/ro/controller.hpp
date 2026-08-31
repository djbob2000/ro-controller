#pragma once

#include <cstdint>
#include <optional>

namespace ro {

enum class Mode : uint8_t { Auto, Maintenance };
enum class State : uint8_t {
    Boot,
    WaitWater,
    Prepare,
    StartupFlush,
    Producing,
    FinalFlush,
    Standby,
    StandbyFlush,
    Maintenance,
    OtaHold,
    ErrorLeak,
    ErrorMaxRuntime,
};
enum class ErrorCode : uint8_t { None, Leak, MaxRuntime };
enum class ContactPolarity : uint8_t { NormallyOpen, NormallyClosed };
inline bool semantic_active(bool raw_high, ContactPolarity polarity) noexcept {
    return polarity == ContactPolarity::NormallyOpen ? raw_high : !raw_high;
}
enum class CommandSource : uint8_t { Local, Web, Mqtt, Scheduler, Internal };
enum class CommandType : uint8_t {
    StartManualFlush,
    ResetError,
    EnterMaintenance,
    ExitMaintenance,
    TestInlet,
    TestPump,
    TestFlush,
    CancelTest,
    EnterOtaHold,
    ExitOtaHold,
};
enum class ServiceTest : uint8_t { None, Inlet, Pump, Flush, ManualFlush };

struct Command {
    CommandType type{};
    CommandSource source{};
};

struct Settings {
    uint64_t boot_stabilize_ms{2500};
    uint64_t low_pressure_stable_ms{3000};
    uint64_t low_pressure_restart_delay_ms{10000};
    uint64_t tank_full_debounce_ms{2000};
    uint64_t prepare_ms{1000};
    uint64_t flush_duration_ms{20000};
    uint64_t long_idle_ms{6ULL * 60 * 60 * 1000};
    uint64_t standby_flush_interval_s{24ULL * 60 * 60};
    uint64_t max_production_ms{3ULL * 60 * 60 * 1000};
    uint64_t service_test_timeout_ms{30000};
    bool standby_flush_enabled{true};
    bool quiet_hours_enabled{true};
    uint16_t quiet_start_minutes{22 * 60};
    uint16_t quiet_end_minutes{8 * 60};

    static Settings defaults() noexcept { return {}; }
};

struct ValidationResult {
    bool ok{true};
    const char* field{nullptr};
};
ValidationResult validate(const Settings& s) noexcept;

struct Inputs {
    bool water_available{false};
    bool tank_full{false};
    bool leak_detected{false};
};

struct TimeInfo {
    bool valid{false};
    int64_t utc_epoch_s{0};
    uint16_t local_minute{0};
};

struct PersistentFacts {
    ErrorCode latched_error{ErrorCode::None};
    std::optional<int64_t> last_membrane_flush_utc_s{};
};

struct DesiredOutputs {
    bool inlet{false};
    bool pump{false};
    bool flush{false};
};

struct SystemSnapshot {
    Mode mode{Mode::Auto};
    State state{State::Boot};
    ErrorCode error{ErrorCode::None};
    DesiredOutputs outputs{};
    Inputs inputs{};
    ServiceTest service_test{ServiceTest::None};
    bool ota_hold{false};
    uint64_t state_age_ms{0};
    uint64_t production_runtime_ms{0};
};

struct ControllerEvents {
    bool state_changed{false};
    State previous_state{State::Boot};
    State new_state{State::Boot};
    bool error_latched{false};
    bool error_cleared{false};
    ErrorCode error{ErrorCode::None};
    bool membrane_flush_completed{false};
    bool production_started{false};
    bool tank_became_full{false};
};

class Controller {
public:
    explicit Controller(Settings settings = Settings::defaults()) noexcept;

    void boot(uint64_t now_ms, const PersistentFacts& facts) noexcept;
    ControllerEvents tick(uint64_t now_ms, const Inputs& inputs, TimeInfo time = {}) noexcept;
    ControllerEvents command(uint64_t now_ms, const Inputs& inputs, const Command& command,
                             TimeInfo time = {}) noexcept;

    [[nodiscard]] const SystemSnapshot& snapshot() const noexcept { return snapshot_; }
    [[nodiscard]] const PersistentFacts& persistent_facts() const noexcept { return facts_; }
    void set_persistent_facts(const PersistentFacts& facts) noexcept;
    void set_settings(const Settings& settings) noexcept;

private:
    Settings settings_{};
    PersistentFacts facts_{};
    SystemSnapshot snapshot_{};
    uint64_t state_entered_ms_{0};
    uint64_t production_started_ms_{0};
    uint64_t water_stable_since_ms_{0};
    uint64_t tank_full_since_ms_{0};
    uint64_t standby_entered_ms_{0};
    uint64_t service_deadline_ms_{0};
    bool water_stability_tracking_{false};
    bool tank_debounce_tracking_{false};
    bool startup_flush_required_{true};
    bool quiet_prefush_consumed_for_window_{false};
    State manual_flush_return_state_{State::Standby};

    void transition(State next, uint64_t now_ms, ControllerEvents& ev) noexcept;
    void apply_outputs(uint64_t now_ms) noexcept;
    bool is_error_state() const noexcept;
    bool is_quiet(uint16_t minute) const noexcept;
    bool preventive_flush_due(TimeInfo time) noexcept;
    void complete_membrane_flush(TimeInfo time, ControllerEvents& ev) noexcept;
    void latch_error(ErrorCode error, uint64_t now_ms, ControllerEvents& ev) noexcept;
    bool clear_error_if_allowed(const Inputs& inputs, uint64_t now_ms, ControllerEvents& ev) noexcept;
};

const char* to_string(State state) noexcept;
const char* to_string(ErrorCode error) noexcept;

} // namespace ro
