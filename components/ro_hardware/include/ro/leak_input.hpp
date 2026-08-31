#pragma once

#include "driver/gpio.h"

namespace ro::hw {

class LeakInput {
public:
    LeakInput(gpio_num_t pin, bool enabled, bool active_low = true) noexcept
        : pin_(pin), enabled_(enabled), active_low_(active_low) {}

    [[nodiscard]] bool available() const noexcept { return enabled_ && pin_ >= GPIO_NUM_0; }
    [[nodiscard]] bool detected() const noexcept {
        if (!available()) return false;
        const bool high = gpio_get_level(pin_) != 0;
        return active_low_ ? !high : high;
    }

private:
    gpio_num_t pin_{GPIO_NUM_NC};
    bool enabled_{false};
    bool active_low_{true};
};

} // namespace ro::hw
