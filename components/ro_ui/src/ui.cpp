#include "ro/ui.hpp"

#include <array>
#include <cstdio>
#include <string_view>

namespace ro::ui {
namespace {

using LineBuffer = std::array<std::array<char, 22>, 8>;

std::array<std::string_view, 8> views(LineBuffer& b) noexcept {
    std::array<std::string_view, 8> out{};
    for (size_t i = 0; i < out.size(); ++i) out[i] = b[i].data();
    return out;
}

const char* on_off(bool value) noexcept { return value ? "ON" : "OFF"; }
const char* yes_no(bool value) noexcept { return value ? "YES" : "NO"; }

const char* test_name(ServiceTest test) noexcept {
    switch (test) {
        case ServiceTest::Inlet: return "INLET";
        case ServiceTest::Pump: return "PUMP";
        case ServiceTest::Flush: return "FLUSH";
        case ServiceTest::ManualFlush: return "MANUAL FLUSH";
        default: return "NONE";
    }
}

} // namespace

void LocalUi::render_status(const SystemSnapshot& s) noexcept {
    LineBuffer b{};
    std::snprintf(b[0].data(), b[0].size(), "RO %s", s.mode == Mode::Auto ? "AUTO" : "MAINT");
    std::snprintf(b[1].data(), b[1].size(), "%s", to_string(s.state));
    std::snprintf(b[3].data(), b[3].size(), "WATER %s", s.inputs.water_available ? "OK" : "LOW");
    std::snprintf(b[4].data(), b[4].size(), "TANK  %s", s.inputs.tank_full ? "FULL" : "FILLING");
    std::snprintf(b[6].data(), b[6].size(), "PUMP %s", on_off(s.outputs.pump));
    std::snprintf(b[7].data(), b[7].size(), "FLUSH %s", on_off(s.outputs.flush));
    hw_.oled_render_lines(views(b));
}

void LocalUi::render_inputs(const SystemSnapshot& s) noexcept {
    LineBuffer b{};
    std::snprintf(b[0].data(), b[0].size(), "INPUTS");
    std::snprintf(b[2].data(), b[2].size(), "LOW P  %s", s.inputs.water_available ? "OK" : "ACTIVE");
    std::snprintf(b[3].data(), b[3].size(), "HIGH P %s", s.inputs.tank_full ? "ACTIVE" : "OFF");
    std::snprintf(b[4].data(), b[4].size(), "LEAK   %s", s.inputs.leak_detected ? "ACTIVE" : "OFF");
    std::snprintf(b[6].data(), b[6].size(), "ERROR  %s", to_string(s.error));
    hw_.oled_render_lines(views(b));
}

void LocalUi::render_outputs(const SystemSnapshot& s) noexcept {
    LineBuffer b{};
    std::snprintf(b[0].data(), b[0].size(), "OUTPUTS");
    std::snprintf(b[2].data(), b[2].size(), "INLET %s", on_off(s.outputs.inlet));
    std::snprintf(b[3].data(), b[3].size(), "PUMP  %s", on_off(s.outputs.pump));
    std::snprintf(b[4].data(), b[4].size(), "FLUSH %s", on_off(s.outputs.flush));
    std::snprintf(b[6].data(), b[6].size(), "RUNTIME %llus",
                  static_cast<unsigned long long>(s.production_runtime_ms / 1000ULL));
    hw_.oled_render_lines(views(b));
}

void LocalUi::render_maintenance(const SystemSnapshot& s) noexcept {
    static constexpr const char* ITEMS[] = {"TEST INLET", "TEST PUMP", "TEST FLUSH", "MANUAL FLUSH"};
    LineBuffer b{};
    std::snprintf(b[0].data(), b[0].size(), "MAINTENANCE");
    std::snprintf(b[1].data(), b[1].size(), "ACTIVE %s", test_name(s.service_test));
    for (uint8_t i = 0; i < 4; ++i) {
        std::snprintf(b[3 + i].data(), b[3 + i].size(), "%c %s", i == maintenance_item_ ? '>' : ' ', ITEMS[i]);
    }
    std::snprintf(b[7].data(), b[7].size(), "HOLD OK TO EXIT");
    hw_.oled_render_lines(views(b));
}

void LocalUi::render_error(const SystemSnapshot& s) noexcept {
    LineBuffer b{};
    std::snprintf(b[0].data(), b[0].size(), "*** ERROR ***");
    std::snprintf(b[2].data(), b[2].size(), "%s", to_string(s.error));
    std::snprintf(b[4].data(), b[4].size(), "PUMP OFF");
    std::snprintf(b[6].data(), b[6].size(), "PRESS OK RESET");
    hw_.oled_render_lines(views(b));
}

void LocalUi::update(uint64_t now_ms, const SystemSnapshot& snapshot,
                     bool time_valid, const char* time_source) noexcept {
    (void)time_valid;
    (void)time_source;
    if (!hw_.oled_available() || now_ms - last_render_ms_ < 200) return;
    last_render_ms_ = now_ms;

    if (snapshot.error != ErrorCode::None) {
        render_error(snapshot);
        return;
    }
    if (snapshot.state == State::Maintenance) {
        render_maintenance(snapshot);
        return;
    }

    switch (page_ % 3U) {
        case 0: render_status(snapshot); break;
        case 1: render_inputs(snapshot); break;
        default: render_outputs(snapshot); break;
    }
}

std::optional<Command> LocalUi::poll_command(uint64_t now_ms, const SystemSnapshot& snapshot) noexcept {
    const auto b = hw_.read_buttons();
    std::optional<Command> command{};

    if (snapshot.error != ErrorCode::None && rising(b.ok, previous_.ok)) {
        command = Command{CommandType::ResetError, CommandSource::Local};
    }

    if (b.ok) {
        if (!previous_.ok) {
            ok_pressed_since_ms_ = now_ms;
            ok_long_consumed_ = false;
        } else if (!ok_long_consumed_ && now_ms - ok_pressed_since_ms_ >= 1500) {
            ok_long_consumed_ = true;
            command = Command{
                snapshot.state == State::Maintenance ? CommandType::ExitMaintenance : CommandType::EnterMaintenance,
                CommandSource::Local};
        }
    } else if (previous_.ok && !ok_long_consumed_ && snapshot.error == ErrorCode::None) {
        if (snapshot.state == State::Maintenance) {
            static constexpr CommandType COMMANDS[] = {
                CommandType::TestInlet,
                CommandType::TestPump,
                CommandType::TestFlush,
                CommandType::StartManualFlush,
            };
            command = Command{COMMANDS[maintenance_item_], CommandSource::Local};
        }
    }

    if (snapshot.state == State::Maintenance) {
        if (rising(b.up, previous_.up)) {
            maintenance_item_ = maintenance_item_ == 0 ? 3 : static_cast<uint8_t>(maintenance_item_ - 1);
        }
        if (rising(b.down, previous_.down)) {
            maintenance_item_ = static_cast<uint8_t>((maintenance_item_ + 1) % 4);
        }
    } else {
        if (rising(b.up, previous_.up)) page_ = static_cast<uint8_t>((page_ + 2) % 3);
        if (rising(b.down, previous_.down)) page_ = static_cast<uint8_t>((page_ + 1) % 3);
    }

    previous_ = b;
    return command;
}

} // namespace ro::ui
