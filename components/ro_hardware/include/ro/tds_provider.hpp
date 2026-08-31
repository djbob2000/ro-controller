#pragma once

#include "ro/controller.hpp"

namespace ro::hw {

class TdsProvider {
public:
    virtual ~TdsProvider() = default;
    [[nodiscard]] virtual OptionalReading ppm() const noexcept = 0;
};

class DisabledTdsProvider final : public TdsProvider {
public:
    [[nodiscard]] OptionalReading ppm() const noexcept override { return {}; }
};

} // namespace ro::hw
