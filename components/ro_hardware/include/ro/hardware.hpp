#pragma once

#include "ro/controller.hpp"
#include "driver/i2c_master.h"
#include "esp_err.h"
#include <array>
#include <cstdint>
#include <string_view>

namespace ro::hw {

struct HardwareConfig {
    ContactPolarity low_pressure_polarity{ContactPolarity::NormallyOpen};
    ContactPolarity high_pressure_polarity{ContactPolarity::NormallyOpen};
    ContactPolarity leak_polarity{ContactPolarity::NormallyOpen};
    bool leak_enabled{false};
    bool inlet_active_high{true};
    bool pump_active_high{true};
    bool flush_active_high{true};
};

struct Buttons {
    bool up{false};
    bool down{false};
    bool ok{false};
};

enum class BootGesture : uint8_t { None, ResetAdmin, FactoryReset };

class Hardware {
public:
    esp_err_t init(const HardwareConfig& cfg) noexcept;
    void update_config(const HardwareConfig& cfg) noexcept;
    Inputs read_inputs() const noexcept;
    Buttons read_buttons() const noexcept;
    void apply_outputs(const DesiredOutputs& outputs) noexcept;
    void force_all_off() noexcept;

    i2c_master_bus_handle_t i2c_bus() const noexcept { return i2c_bus_; }
    bool oled_available() const noexcept { return oled_dev_ != nullptr; }
    bool i2c_available() const noexcept { return i2c_bus_ != nullptr; }

    esp_err_t oled_clear() noexcept;
    esp_err_t oled_render_lines(const std::array<std::string_view, 8>& lines) noexcept;
    esp_err_t oled_show_message(std::string_view a, std::string_view b = {}) noexcept;

private:
    HardwareConfig cfg_{};
    i2c_master_bus_handle_t i2c_bus_{nullptr};
    i2c_master_dev_handle_t oled_dev_{nullptr};
    std::array<uint8_t, 1024> framebuffer_{};

    esp_err_t init_gpio() noexcept;
    esp_err_t init_i2c() noexcept;
    esp_err_t init_oled() noexcept;
    void set_output(int gpio, bool logical_on, bool active_high) noexcept;
    bool contact_closed(int gpio) const noexcept;
    esp_err_t oled_command(uint8_t cmd) noexcept;
    esp_err_t oled_flush() noexcept;
    void draw_char(int x, int page, char ch) noexcept;
};

BootGesture detect_boot_gesture(Hardware& hw, uint32_t sample_ms = 50) noexcept;
bool confirm_reset(Hardware& hw, BootGesture gesture) noexcept;

} // namespace ro::hw
