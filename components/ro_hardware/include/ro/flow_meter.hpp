#pragma once

#include "ro/controller.hpp"
#include "driver/gpio.h"
#include "driver/pulse_cnt.h"

#include <cstdint>

namespace ro::hw {

class FlowMeter {
public:
    virtual ~FlowMeter() = default;
    [[nodiscard]] virtual OptionalReading rate_lpm() const noexcept = 0;
    [[nodiscard]] virtual OptionalReading total_liters() const noexcept = 0;
};

class DisabledFlowMeter final : public FlowMeter {
public:
    [[nodiscard]] OptionalReading rate_lpm() const noexcept override { return {}; }
    [[nodiscard]] OptionalReading total_liters() const noexcept override { return {}; }
};

class PcntFlowMeter final : public FlowMeter {
public:
    PcntFlowMeter(gpio_num_t signal_pin, float pulses_per_liter) noexcept;
    ~PcntFlowMeter() override;

    PcntFlowMeter(const PcntFlowMeter&) = delete;
    PcntFlowMeter& operator=(const PcntFlowMeter&) = delete;

    [[nodiscard]] bool available() const noexcept { return unit_ != nullptr && pulses_per_liter_ > 0.0F; }
    [[nodiscard]] OptionalReading rate_lpm() const noexcept override;
    [[nodiscard]] OptionalReading total_liters() const noexcept override;

private:
    pcnt_unit_handle_t unit_{nullptr};
    pcnt_channel_handle_t channel_{nullptr};
    float pulses_per_liter_{0.0F};
    mutable int last_count_{0};
    mutable int64_t last_sample_us_{0};
    mutable uint64_t accumulated_pulses_{0};

    void sample(int& count, int64_t& now_us) const noexcept;
};

} // namespace ro::hw
