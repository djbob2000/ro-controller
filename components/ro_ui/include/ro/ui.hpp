#pragma once

#include "ro/controller.hpp"
#include "ro/hardware.hpp"

#include <cstdint>
#include <optional>

namespace ro::ui {

class LocalUi {
public:
    explicit LocalUi(hw::Hardware& hardware) noexcept : hw_(hardware) {}

    void update(uint64_t now_ms, const SystemSnapshot& snapshot,
                bool time_valid = false, const char* time_source = "--") noexcept;
    std::optional<Command> poll_command(uint64_t now_ms, const SystemSnapshot& snapshot) noexcept;

private:
    hw::Hardware& hw_;
    uint8_t page_{0};
    uint8_t maintenance_item_{0};
    hw::Buttons previous_{};
    uint64_t ok_pressed_since_ms_{0};
    bool ok_long_consumed_{false};
    uint64_t last_render_ms_{0};

    bool rising(bool current, bool previous) const noexcept { return current && !previous; }
    void render_status(const SystemSnapshot& snapshot) noexcept;
    void render_inputs(const SystemSnapshot& snapshot) noexcept;
    void render_outputs(const SystemSnapshot& snapshot) noexcept;
    void render_maintenance(const SystemSnapshot& snapshot) noexcept;
    void render_error(const SystemSnapshot& snapshot) noexcept;
};

} // namespace ro::ui
