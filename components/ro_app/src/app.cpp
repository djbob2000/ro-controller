#include "ro/app.hpp"

#include "ro/controller.hpp"
#include "ro/hardware.hpp"
#include "ro/network.hpp"
#include "ro/services.hpp"
#include "ro/ui.hpp"

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <optional>
#include <string>

namespace ro::app {
namespace {
constexpr char TAG[] = "ro_app";
constexpr TickType_t CONTROL_PERIOD = pdMS_TO_TICKS(20);
constexpr TickType_t UI_PERIOD = pdMS_TO_TICKS(50);

uint64_t monotonic_ms() noexcept {
    return static_cast<uint64_t>(esp_timer_get_time() / 1000);
}

bool same_facts(const PersistentFacts& a, const PersistentFacts& b) noexcept {
    return a.latched_error == b.latched_error && a.last_membrane_flush_utc_s == b.last_membrane_flush_utc_s;
}

class Runtime {
public:
    Runtime() noexcept
        : config_(svc::AppConfig::defaults()), controller_(config_.controller),
          filters_(store_, config_, filter_states_), ui_(hardware_) {}

    esp_err_t init() noexcept {
        ESP_RETURN_ON_ERROR(store_.init(), TAG, "storage init failed");
        ESP_RETURN_ON_ERROR(store_.load(config_, facts_, filter_states_), TAG, "storage load failed");
        ESP_RETURN_ON_ERROR(store_.load_admin(config_.admin), TAG, "admin load failed");

        ESP_RETURN_ON_ERROR(hardware_.init(config_.hardware), TAG, "hardware init failed");
        hardware_.force_all_off();

        const auto gesture = hw::detect_boot_gesture(hardware_);
        if (gesture != hw::BootGesture::None && hw::confirm_reset(hardware_, gesture)) {
            hardware_.force_all_off();
            if (gesture == hw::BootGesture::FactoryReset) {
                ESP_LOGW(TAG, "factory reset requested");
                store_.factory_reset();
            } else {
                ESP_LOGW(TAG, "administrator reset requested");
                store_.save_admin({});
            }
            vTaskDelay(pdMS_TO_TICKS(500));
            esp_restart();
        }

        controller_.set_settings(config_.controller);
        controller_.boot(monotonic_ms(), facts_);
        published_snapshot_ = controller_.snapshot();

        ESP_RETURN_ON_ERROR(time_.init(hardware_.i2c_bus(), config_), TAG, "time init failed");
        ESP_RETURN_ON_ERROR(stats_.init(), TAG, "statistics init failed");

        command_queue_ = xQueueCreate(16, sizeof(Command));
        snapshot_mutex_ = xSemaphoreCreateMutex();
        if (!command_queue_ || !snapshot_mutex_) return ESP_ERR_NO_MEM;

        net::Hooks hooks{};
        hooks.context = this;
        hooks.snapshot = &Runtime::hook_snapshot;
        hooks.enqueue_command = &Runtime::hook_enqueue;
        hooks.set_ota_hold = &Runtime::hook_ota_hold;
        hooks.apply_config = &Runtime::hook_apply_config;
        hooks.set_admin_password = &Runtime::hook_set_admin_password;
        hooks.reset_filter = &Runtime::hook_reset_filter;
        network_.emplace(store_, config_, time_, events_, stats_, filters_, hooks);
        const esp_err_t net_err = network_->init();
        if (net_err != ESP_OK) ESP_LOGW(TAG, "network unavailable: %s", esp_err_to_name(net_err));

        ota_validation_deadline_ms_ = monotonic_ms() + 10'000;
        if (xTaskCreatePinnedToCore(&Runtime::control_task_entry, "ro_control", 8192, this, 12,
                                    &control_task_, 1) != pdPASS) return ESP_ERR_NO_MEM;
        if (xTaskCreatePinnedToCore(&Runtime::ui_task_entry, "ro_ui", 6144, this, 5,
                                    &ui_task_, 0) != pdPASS) return ESP_ERR_NO_MEM;
        return ESP_OK;
    }

private:
    svc::Store store_{};
    svc::AppConfig config_{};
    PersistentFacts facts_{};
    std::array<svc::FilterState,5> filter_states_{};
    hw::Hardware hardware_{};
    Controller controller_;
    svc::TimeService time_{};
    svc::EventLog events_{};
    svc::StatisticsService stats_{};
    svc::FilterService filters_;
    ui::LocalUi ui_;
    std::optional<net::NetworkManager> network_{};

    QueueHandle_t command_queue_{nullptr};
    SemaphoreHandle_t snapshot_mutex_{nullptr};
    TaskHandle_t control_task_{nullptr};
    TaskHandle_t ui_task_{nullptr};
    SystemSnapshot published_snapshot_{};
    std::atomic<uint64_t> reboot_at_ms_{0};
    uint64_t ota_validation_deadline_ms_{0};
    bool ota_validated_{false};
    uint64_t last_stats_second_{0};

    static SystemSnapshot hook_snapshot(void* context) noexcept {
        return static_cast<Runtime*>(context)->snapshot();
    }

    static bool hook_enqueue(void* context, const Command& command) noexcept {
        return static_cast<Runtime*>(context)->enqueue(command);
    }

    static bool hook_ota_hold(void* context, bool enabled) noexcept {
        return static_cast<Runtime*>(context)->set_ota_hold(enabled);
    }

    static esp_err_t hook_apply_config(void* context, const std::string& json) noexcept {
        return static_cast<Runtime*>(context)->apply_config(json);
    }

    static esp_err_t hook_set_admin_password(void* context, const std::string& password) noexcept {
        return static_cast<Runtime*>(context)->set_admin_password(password);
    }

    static esp_err_t hook_reset_filter(void* context, size_t index) noexcept {
        return static_cast<Runtime*>(context)->reset_filter(index);
    }

    SystemSnapshot snapshot() noexcept {
        SystemSnapshot copy{};
        if (!snapshot_mutex_) return published_snapshot_;
        if (xSemaphoreTake(snapshot_mutex_, pdMS_TO_TICKS(100)) == pdTRUE) {
            copy = published_snapshot_;
            xSemaphoreGive(snapshot_mutex_);
        }
        return copy;
    }

    void publish_snapshot() noexcept {
        if (!snapshot_mutex_) return;
        if (xSemaphoreTake(snapshot_mutex_, pdMS_TO_TICKS(20)) == pdTRUE) {
            published_snapshot_ = controller_.snapshot();
            xSemaphoreGive(snapshot_mutex_);
        }
    }

    bool enqueue(const Command& command) noexcept {
        return command_queue_ && xQueueSend(command_queue_, &command, 0) == pdTRUE;
    }

    bool set_ota_hold(bool enabled) noexcept {
        const Command cmd{
            enabled ? CommandType::EnterOtaHold : CommandType::ExitOtaHold,
            CommandSource::Internal,
        };
        if (!enqueue(cmd)) return false;
        const uint64_t deadline = monotonic_ms() + 1000;
        while (monotonic_ms() < deadline) {
            const auto s = snapshot();
            if (enabled && s.state == State::OtaHold && !s.outputs.inlet && !s.outputs.pump && !s.outputs.flush)
                return true;
            if (!enabled && s.state != State::OtaHold) return true;
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        return false;
    }

    esp_err_t apply_config(const std::string& json) noexcept {
        svc::AppConfig next = config_;
        const esp_err_t err = store_.apply_config_json(json, next, true);
        if (err != ESP_OK) return err;
        // Hardware/network configuration is activated only after a clean reboot.
        // This prevents polarity changes or network teardown from racing the control loop.
        ESP_RETURN_ON_ERROR(store_.save_config(next), TAG, "save config failed");
        reboot_at_ms_.store(monotonic_ms() + 1500, std::memory_order_release);
        return ESP_OK;
    }

    esp_err_t set_admin_password(const std::string& password) noexcept {
        svc::AppConfig temporary = config_;
        if (!svc::Security::set_admin_password(temporary, password)) return ESP_ERR_INVALID_ARG;
        return store_.save_admin(temporary.admin);
    }

    esp_err_t reset_filter(size_t index) noexcept {
        const auto t = time_.now();
        if (!t.valid) return ESP_ERR_INVALID_STATE;
        return filters_.reset(index, t.utc_epoch_s);
    }

    void persist_events(const ControllerEvents& ev, TimeInfo time) noexcept {
        const int64_t epoch = time.valid ? time.utc_epoch_s : 0;
        if (ev.state_changed) {
            char message[96];
            std::snprintf(message, sizeof(message), "state %s -> %s",
                          to_string(ev.previous_state), to_string(ev.new_state));
            events_.append(epoch, message);
            stats_.on_transition(ev.previous_state, ev.new_state, epoch);
        }
        if (ev.error_latched) {
            events_.append(epoch, std::string("error latched: ") + to_string(ev.error));
        }
        if (ev.error_cleared) events_.append(epoch, "error reset");
        if (ev.membrane_flush_completed) events_.append(epoch, "membrane flush completed");

        const auto& current = controller_.persistent_facts();
        if (!same_facts(facts_, current)) {
            facts_ = current;
            const esp_err_t err = store_.save_facts(facts_);
            if (err != ESP_OK) ESP_LOGE(TAG, "unable to persist controller facts: %s", esp_err_to_name(err));
        }
    }

    void validate_ota_if_ready(uint64_t now_ms) noexcept {
        if (ota_validated_ || now_ms < ota_validation_deadline_ms_) return;
        const esp_partition_t* running = esp_ota_get_running_partition();
        esp_ota_img_states_t state{};
        if (running && esp_ota_get_state_partition(running, &state) == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY) {
            if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
                ESP_LOGI(TAG, "OTA image marked valid after stable runtime");
                events_.append(time_.now().utc_epoch_s, "OTA image validated");
            }
        }
        ota_validated_ = true;
    }

    void maybe_reboot(uint64_t now_ms) noexcept {
        const uint64_t requested = reboot_at_ms_.load(std::memory_order_acquire);
        if (requested == 0 || now_ms < requested) return;
        hardware_.force_all_off();
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_restart();
    }

    void control_loop() noexcept {
        esp_task_wdt_add(nullptr);
        TickType_t wake = xTaskGetTickCount();
        for (;;) {
            const uint64_t now_ms = monotonic_ms();
            const Inputs inputs = hardware_.read_inputs();
            const TimeInfo current_time = time_.now();

            Command cmd{};
            while (xQueueReceive(command_queue_, &cmd, 0) == pdTRUE) {
                const ControllerEvents ev = controller_.command(now_ms, inputs, cmd, current_time);
                persist_events(ev, current_time);
            }

            const ControllerEvents ev = controller_.tick(now_ms, inputs, current_time);
            persist_events(ev, current_time);
            hardware_.apply_outputs(controller_.snapshot().outputs);
            publish_snapshot();

            if (current_time.valid && static_cast<uint64_t>(current_time.utc_epoch_s) != last_stats_second_) {
                stats_.observe_second(controller_.snapshot(), current_time.utc_epoch_s);
                last_stats_second_ = static_cast<uint64_t>(current_time.utc_epoch_s);
            }

            validate_ota_if_ready(now_ms);
            maybe_reboot(now_ms);
            esp_task_wdt_reset();
            vTaskDelayUntil(&wake, CONTROL_PERIOD);
        }
    }

    void ui_loop() noexcept {
        for (;;) {
            const uint64_t now_ms = monotonic_ms();
            const auto snap = snapshot();
            if (auto command = ui_.poll_command(now_ms, snap)) enqueue(*command);
            ui_.update(now_ms, snap, time_.now().valid, time_.source_name());
            if (network_) network_->publish_periodic(now_ms);
            vTaskDelay(UI_PERIOD);
        }
    }

    static void control_task_entry(void* arg) {
        static_cast<Runtime*>(arg)->control_loop();
    }

    static void ui_task_entry(void* arg) {
        static_cast<Runtime*>(arg)->ui_loop();
    }
};

Runtime runtime;
}

void start() {
    const esp_err_t err = runtime.init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "fatal initialization error: %s", esp_err_to_name(err));
        // Hardware initialization itself forces outputs off. If later initialization fails,
        // never spin up automatic production; restart and retry in a safe state.
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_restart();
    }
}

} // namespace ro::app
