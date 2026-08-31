#pragma once

#include "ro/controller.hpp"

namespace ro::hw {

class RecoveryDriver {
public:
    virtual ~RecoveryDriver() = default;
    [[nodiscard]] virtual OptionalReading recovery_percent() const noexcept = 0;
    virtual bool set_target_percent(float target) noexcept = 0;
};

class DisabledRecoveryDriver final : public RecoveryDriver {
public:
    [[nodiscard]] OptionalReading recovery_percent() const noexcept override { return {}; }
    bool set_target_percent(float) noexcept override { return false; }
};

} // namespace ro::hw
