#include "ro/hardware.hpp"

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <algorithm>
#include <array>
#include <cstring>

namespace ro::hw {
namespace {
constexpr char TAG[] = "ro_hw";
constexpr gpio_num_t PIN_LOW = GPIO_NUM_6;
constexpr gpio_num_t PIN_HIGH = GPIO_NUM_7;
constexpr gpio_num_t PIN_SDA = GPIO_NUM_8;
constexpr gpio_num_t PIN_SCL = GPIO_NUM_9;
constexpr gpio_num_t PIN_UP = GPIO_NUM_10;
constexpr gpio_num_t PIN_DOWN = GPIO_NUM_11;
constexpr gpio_num_t PIN_OK = GPIO_NUM_12;
constexpr gpio_num_t PIN_INLET = GPIO_NUM_13;
constexpr gpio_num_t PIN_PUMP = GPIO_NUM_14;
constexpr gpio_num_t PIN_FLUSH = GPIO_NUM_15;
constexpr gpio_num_t PIN_LEAK = GPIO_NUM_38;
constexpr uint8_t OLED_ADDR = 0x3C;

struct Glyph { char c; std::array<uint8_t,5> p; };
constexpr Glyph FONT[] = {
 {' ',{0,0,0,0,0}}, {'-',{0x08,0x08,0x08,0x08,0x08}}, {'.',{0,0x60,0x60,0,0}},
 {':',{0,0x36,0x36,0,0}}, {'/',{0x20,0x10,0x08,0x04,0x02}}, {'%',{0x62,0x64,0x08,0x13,0x23}},
 {'0',{0x3E,0x51,0x49,0x45,0x3E}}, {'1',{0,0x42,0x7F,0x40,0}}, {'2',{0x42,0x61,0x51,0x49,0x46}},
 {'3',{0x21,0x41,0x45,0x4B,0x31}}, {'4',{0x18,0x14,0x12,0x7F,0x10}}, {'5',{0x27,0x45,0x45,0x45,0x39}},
 {'6',{0x3C,0x4A,0x49,0x49,0x30}}, {'7',{0x01,0x71,0x09,0x05,0x03}}, {'8',{0x36,0x49,0x49,0x49,0x36}},
 {'9',{0x06,0x49,0x49,0x29,0x1E}},
 {'A',{0x7E,0x11,0x11,0x11,0x7E}}, {'B',{0x7F,0x49,0x49,0x49,0x36}}, {'C',{0x3E,0x41,0x41,0x41,0x22}},
 {'D',{0x7F,0x41,0x41,0x22,0x1C}}, {'E',{0x7F,0x49,0x49,0x49,0x41}}, {'F',{0x7F,0x09,0x09,0x09,0x01}},
 {'G',{0x3E,0x41,0x49,0x49,0x7A}}, {'H',{0x7F,0x08,0x08,0x08,0x7F}}, {'I',{0,0x41,0x7F,0x41,0}},
 {'J',{0x20,0x40,0x41,0x3F,0x01}}, {'K',{0x7F,0x08,0x14,0x22,0x41}}, {'L',{0x7F,0x40,0x40,0x40,0x40}},
 {'M',{0x7F,0x02,0x0C,0x02,0x7F}}, {'N',{0x7F,0x04,0x08,0x10,0x7F}}, {'O',{0x3E,0x41,0x41,0x41,0x3E}},
 {'P',{0x7F,0x09,0x09,0x09,0x06}}, {'Q',{0x3E,0x41,0x51,0x21,0x5E}}, {'R',{0x7F,0x09,0x19,0x29,0x46}},
 {'S',{0x46,0x49,0x49,0x49,0x31}}, {'T',{0x01,0x01,0x7F,0x01,0x01}}, {'U',{0x3F,0x40,0x40,0x40,0x3F}},
 {'V',{0x1F,0x20,0x40,0x20,0x1F}}, {'W',{0x3F,0x40,0x38,0x40,0x3F}}, {'X',{0x63,0x14,0x08,0x14,0x63}},
 {'Y',{0x07,0x08,0x70,0x08,0x07}}, {'Z',{0x61,0x51,0x49,0x45,0x43}},
 {'_',{0x40,0x40,0x40,0x40,0x40}}, {'?',{0x02,0x01,0x51,0x09,0x06}}
};

std::array<uint8_t,5> glyph(char c) {
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    for (const auto& g : FONT) if (g.c == c) return g.p;
    return {0x02,0x01,0x51,0x09,0x06};
}
}

esp_err_t Hardware::init(const HardwareConfig& cfg) noexcept {
    cfg_ = cfg;
    ESP_RETURN_ON_ERROR(init_gpio(), TAG, "gpio init failed");
    auto err = init_i2c();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "I2C unavailable: %s", esp_err_to_name(err));
        return ESP_OK;
    }
    err = init_oled();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SSD1306 not found: %s", esp_err_to_name(err));
        oled_dev_ = nullptr;
    }
    return ESP_OK;
}

void Hardware::update_config(const HardwareConfig& cfg) noexcept {
    force_all_off();
    cfg_ = cfg;
    force_all_off();
}

esp_err_t Hardware::init_gpio() noexcept {
    gpio_config_t out{};
    out.pin_bit_mask = (1ULL<<PIN_INLET) | (1ULL<<PIN_PUMP) | (1ULL<<PIN_FLUSH);
    out.mode = GPIO_MODE_OUTPUT;
    out.pull_down_en = GPIO_PULLDOWN_ENABLE;
    out.pull_up_en = GPIO_PULLUP_DISABLE;
    out.intr_type = GPIO_INTR_DISABLE;
    ESP_RETURN_ON_ERROR(gpio_config(&out), TAG, "output gpio config");
    force_all_off();

    gpio_config_t in{};
    in.pin_bit_mask = (1ULL<<PIN_LOW) | (1ULL<<PIN_HIGH) | (1ULL<<PIN_LEAK) |
                      (1ULL<<PIN_UP) | (1ULL<<PIN_DOWN) | (1ULL<<PIN_OK);
    in.mode = GPIO_MODE_INPUT;
    in.pull_up_en = GPIO_PULLUP_ENABLE;
    in.pull_down_en = GPIO_PULLDOWN_DISABLE;
    in.intr_type = GPIO_INTR_DISABLE;
    return gpio_config(&in);
}

esp_err_t Hardware::init_i2c() noexcept {
    i2c_master_bus_config_t cfg{};
    cfg.i2c_port = I2C_NUM_0;
    cfg.sda_io_num = PIN_SDA;
    cfg.scl_io_num = PIN_SCL;
    cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    cfg.glitch_ignore_cnt = 7;
    cfg.flags.enable_internal_pullup = true;
    return i2c_new_master_bus(&cfg, &i2c_bus_);
}

esp_err_t Hardware::init_oled() noexcept {
    i2c_device_config_t dev{};
    dev.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev.device_address = OLED_ADDR;
    dev.scl_speed_hz = 400000;
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(i2c_bus_, &dev, &oled_dev_), TAG, "add OLED");
    constexpr uint8_t init_cmds[] = {
        0xAE, 0x20, 0x00, 0xB0, 0xC8, 0x00, 0x10, 0x40,
        0x81, 0x7F, 0xA1, 0xA6, 0xA8, 0x3F, 0xA4, 0xD3,
        0x00, 0xD5, 0x80, 0xD9, 0xF1, 0xDA, 0x12, 0xDB,
        0x40, 0x8D, 0x14, 0xAF
    };
    for (auto c : init_cmds) ESP_RETURN_ON_ERROR(oled_command(c), TAG, "OLED command");
    return oled_clear();
}

void Hardware::set_output(int gpio, bool logical_on, bool active_high) noexcept {
    const int level = logical_on == active_high ? 1 : 0;
    gpio_set_level(static_cast<gpio_num_t>(gpio), level);
}

void Hardware::force_all_off() noexcept {
    set_output(PIN_INLET, false, cfg_.inlet_active_high);
    set_output(PIN_PUMP, false, cfg_.pump_active_high);
    set_output(PIN_FLUSH, false, cfg_.flush_active_high);
}

void Hardware::apply_outputs(const DesiredOutputs& outputs) noexcept {
    set_output(PIN_INLET, outputs.inlet, cfg_.inlet_active_high);
    set_output(PIN_PUMP, outputs.pump, cfg_.pump_active_high);
    set_output(PIN_FLUSH, outputs.flush, cfg_.flush_active_high);
}

bool Hardware::contact_closed(int gpio) const noexcept {
    return gpio_get_level(static_cast<gpio_num_t>(gpio)) == 0;
}

Inputs Hardware::read_inputs() const noexcept {
    Inputs in{};
    in.water_available = semantic_active(contact_closed(PIN_LOW), cfg_.low_pressure_polarity);
    in.tank_full = semantic_active(contact_closed(PIN_HIGH), cfg_.high_pressure_polarity);
    in.leak_detected = cfg_.leak_enabled && semantic_active(contact_closed(PIN_LEAK), cfg_.leak_polarity);
    return in;
}

Buttons Hardware::read_buttons() const noexcept {
    return {
        gpio_get_level(PIN_UP) == 0,
        gpio_get_level(PIN_DOWN) == 0,
        gpio_get_level(PIN_OK) == 0,
    };
}

esp_err_t Hardware::oled_command(uint8_t cmd) noexcept {
    if (!oled_dev_) return ESP_ERR_INVALID_STATE;
    uint8_t b[2]{0x00, cmd};
    return i2c_master_transmit(oled_dev_, b, sizeof(b), 100);
}

esp_err_t Hardware::oled_flush() noexcept {
    if (!oled_dev_) return ESP_ERR_INVALID_STATE;
    ESP_RETURN_ON_ERROR(oled_command(0x21), TAG, "col cmd");
    ESP_RETURN_ON_ERROR(oled_command(0), TAG, "col start");
    ESP_RETURN_ON_ERROR(oled_command(127), TAG, "col end");
    ESP_RETURN_ON_ERROR(oled_command(0x22), TAG, "page cmd");
    ESP_RETURN_ON_ERROR(oled_command(0), TAG, "page start");
    ESP_RETURN_ON_ERROR(oled_command(7), TAG, "page end");
    std::array<uint8_t, 17> packet{};
    packet[0] = 0x40;
    for (size_t off = 0; off < framebuffer_.size(); off += 16) {
        std::memcpy(packet.data()+1, framebuffer_.data()+off, 16);
        ESP_RETURN_ON_ERROR(i2c_master_transmit(oled_dev_, packet.data(), packet.size(), 100), TAG, "OLED data");
    }
    return ESP_OK;
}

esp_err_t Hardware::oled_clear() noexcept {
    framebuffer_.fill(0);
    return oled_flush();
}

void Hardware::draw_char(int x, int page, char ch) noexcept {
    if (page < 0 || page > 7 || x < 0 || x > 122) return;
    const auto bits = glyph(ch);
    auto offset = static_cast<size_t>(page * 128 + x);
    for (int i=0; i<5; ++i) framebuffer_[offset+i] = bits[i];
    framebuffer_[offset+5] = 0;
}

esp_err_t Hardware::oled_render_lines(const std::array<std::string_view,8>& lines) noexcept {
    if (!oled_dev_) return ESP_ERR_INVALID_STATE;
    framebuffer_.fill(0);
    for (int page=0; page<8; ++page) {
        int x=0;
        for (char c : lines[page]) {
            if (x > 122) break;
            draw_char(x,page,c);
            x += 6;
        }
    }
    return oled_flush();
}

esp_err_t Hardware::oled_show_message(std::string_view a, std::string_view b) noexcept {
    std::array<std::string_view,8> lines{};
    lines[2]=a; lines[4]=b;
    return oled_render_lines(lines);
}

BootGesture detect_boot_gesture(Hardware& hw, uint32_t sample_ms) noexcept {
    const auto b = hw.read_buttons();
    if (!(b.up && b.down)) return BootGesture::None;
    const bool factory = b.ok;
    const uint32_t hold_ms = factory ? 10'000 : 5'000;
    uint32_t elapsed = 0;
    while (elapsed < hold_ms) {
        vTaskDelay(pdMS_TO_TICKS(sample_ms));
        elapsed += sample_ms;
        const auto now = hw.read_buttons();
        if (!now.up || !now.down || (factory && !now.ok)) return BootGesture::None;
    }
    return factory ? BootGesture::FactoryReset : BootGesture::ResetAdmin;
}

bool confirm_reset(Hardware& hw, BootGesture gesture) noexcept {
    if (gesture == BootGesture::None) return false;
    hw.oled_show_message(gesture == BootGesture::FactoryReset ? "FACTORY RESET" : "RESET ADMIN", "HOLD OK 3 SEC");
    uint32_t held=0;
    while (held < 3000) {
        vTaskDelay(pdMS_TO_TICKS(50));
        const auto b=hw.read_buttons();
        if (b.ok && !b.up && !b.down) held += 50;
        else held=0;
    }
    hw.oled_show_message("RESET COMPLETE", "RESTARTING");
    return true;
}

} // namespace ro::hw
