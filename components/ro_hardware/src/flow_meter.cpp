#include "ro/flow_meter.hpp"

#include "esp_timer.h"

#include <algorithm>

namespace ro::hw {

PcntFlowMeter::PcntFlowMeter(gpio_num_t signal_pin, float pulses_per_liter) noexcept
    : pulses_per_liter_(pulses_per_liter) {
    if (signal_pin < GPIO_NUM_0 || pulses_per_liter_ <= 0.0F) return;

    pcnt_unit_config_t unit_config{};
    unit_config.low_limit = -1;
    unit_config.high_limit = 32767;
    if (pcnt_new_unit(&unit_config, &unit_) != ESP_OK) {
        unit_ = nullptr;
        return;
    }

    pcnt_chan_config_t channel_config{};
    channel_config.edge_gpio_num = signal_pin;
    channel_config.level_gpio_num = -1;
    if (pcnt_new_channel(unit_, &channel_config, &channel_) != ESP_OK) {
        pcnt_del_unit(unit_);
        unit_ = nullptr;
        return;
    }

    if (pcnt_channel_set_edge_action(channel_, PCNT_CHANNEL_EDGE_ACTION_INCREASE,
                                     PCNT_CHANNEL_EDGE_ACTION_HOLD) != ESP_OK ||
        pcnt_unit_enable(unit_) != ESP_OK || pcnt_unit_clear_count(unit_) != ESP_OK ||
        pcnt_unit_start(unit_) != ESP_OK) {
        pcnt_del_channel(channel_);
        channel_ = nullptr;
        pcnt_del_unit(unit_);
        unit_ = nullptr;
        return;
    }
    last_sample_us_ = esp_timer_get_time();
}

PcntFlowMeter::~PcntFlowMeter() {
    if (unit_) {
        pcnt_unit_stop(unit_);
        pcnt_unit_disable(unit_);
    }
    if (channel_) pcnt_del_channel(channel_);
    if (unit_) pcnt_del_unit(unit_);
}

void PcntFlowMeter::sample(int& count, int64_t& now_us) const noexcept {
    count = last_count_;
    now_us = esp_timer_get_time();
    if (!available()) return;
    int current = 0;
    if (pcnt_unit_get_count(unit_, &current) != ESP_OK) return;

    // The counter is cleared after every observation so a high pulse rate cannot
    // silently wrap the hardware counter between ordinary UI/network samples.
    if (current > 0) accumulated_pulses_ += static_cast<uint64_t>(current);
    pcnt_unit_clear_count(unit_);
    last_count_ = current;
}

OptionalReading PcntFlowMeter::rate_lpm() const noexcept {
    if (!available()) return {};
    int count = 0;
    int64_t now_us = 0;
    const int64_t previous_us = last_sample_us_;
    sample(count, now_us);
    const int64_t elapsed_us = now_us - previous_us;
    last_sample_us_ = now_us;
    if (elapsed_us <= 0) return {true, 0.0F};
    const float liters = static_cast<float>(std::max(count, 0)) / pulses_per_liter_;
    return {true, liters * (60.0F * 1'000'000.0F / static_cast<float>(elapsed_us))};
}

OptionalReading PcntFlowMeter::total_liters() const noexcept {
    if (!available()) return {};
    int count = 0;
    int64_t now_us = 0;
    sample(count, now_us);
    last_sample_us_ = now_us;
    return {true, static_cast<float>(accumulated_pulses_) / pulses_per_liter_};
}

} // namespace ro::hw
