#include "ro/app.hpp"

#include "ro/controller.hpp"
#include "ro/flow_meter.hpp"
#include "ro/hardware.hpp"
#include "ro/network.hpp"
#include "ro/services.hpp"
#include "ro/ui.hpp"

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>

namespace ro::app {
namespace {
constexpr char TAG[] = "ro_app";
constexpr TickType_t CONTROL_PERIOD = pdMS_TO_TICKS(20);
constexpr TickType_t UI_PERIOD = pdMS_TO_TICKS(50);
constexpr uint64_t STATS_CHECKPOINT_MS = 5ULL * 60ULL * 1000ULL;
constexpr std::array<gpio_num_t, 3> FLOW_PINS{GPIO_NUM_4, GPIO_NUM_5, GPIO_NUM_16};

uint64_t monotonic_ms() noexcept {
    return static_cast<uint64_t>(esp_timer_get_time() / 1000);
}

int64_t local_calendar_epoch(int64_t utc_epoch) noexcept {
    if (utc_epoch <= 0) return 0;
    const time_t utc = static_cast<time_t>(utc_epoch);
    std::tm local{};
    localtime_r(&utc, &local);
    return static_cast<int64_t>(timegm(&local));
}

void preload_safe_output_latches(const hw::HardwareConfig& cfg) noexcept {
    // ESP32 GPIOs start high-impedance. Load the OFF level into the output
    // latch before Hardware::init() switches the pins to output mode so an
    // active-low relay/MOSFET cannot receive a short ON pulse during boot.
    gpio_set_level(GPIO_NUM_13, cfg.inlet_active_high ? 0 : 1);
    gpio_set_level(GPIO_NUM_14, cfg.pump_active_high ? 0 : 1);
    gpio_set_level(GPIO_NUM_15, cfg.flush_active_high ? 0 : 1);
}

bool same_facts(const PersistentFacts& a, const PersistentFacts& b) noexcept {
    return a.latched_error == b.latched_error && a.last_membrane_flush_utc_s == b.last_membrane_flush_utc_s;
}

bool has_event(const ControllerEvents& ev) noexcept {
    return ev.state_changed || ev.error_latched || ev.error_cleared || ev.membrane_flush_completed ||
           ev.production_started || ev.tank_became_full;
}

struct EventWork {
    ControllerEvents events{};
    TimeInfo time{};
};

struct StatsWork {
    SystemSnapshot snapshot{};
    int64_t epoch{0};
};

struct LogWork {
    int64_t epoch{0};
    char message[96]{};
};

static_assert(std::is_trivially_copyable_v<EventWork>);
static_assert(std::is_trivially_copyable_v<StatsWork>);
static_assert(std::is_trivially_copyable_v<LogWork>);
static_assert(std::is_trivially_copyable_v<PersistentFacts>);

class Runtime {
public:
    Runtime() noexcept
        : config_(svc::AppConfig::defaults()), controller_(config_.controller),
          filters_(store_, config_, filter_states_), ui_(hardware_) {}

    esp_err_t init() noexcept {
        ESP_RETURN_ON_ERROR(store_.init(), TAG, "storage init failed");
        ESP_RETURN_ON_ERROR(store_.load(config_, facts_, filter_states_), TAG, "storage load failed");
        ESP_RETURN_ON_ERROR(store_.load_admin(config_.admin), TAG, "admin load failed");
        queued_facts_ = facts_;

        preload_safe_output_latches(config_.hardware);
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
        init_optional_hardware();

        command_queue_ = xQueueCreate(16, sizeof(Command));
        event_queue_ = xQueueCreate(32, sizeof(EventWork));
        facts_queue_ = xQueueCreate(1, sizeof(PersistentFacts));
        stats_queue_ = xQueueCreate(16, sizeof(StatsWork));
        log_queue_ = xQueueCreate(16, sizeof(LogWork));
        snapshot_mutex_ = xSemaphoreCreateMutex();
        if (!command_queue_ || !event_queue_ || !facts_queue_ || !stats_queue_ || !log_queue_ || !snapshot_mutex_)
            return ESP_ERR_NO_MEM;

        if (xTaskCreatePinnedToCore(&Runtime::storage_task_entry, "ro_storage", 6144, this, 3,
                                    &storage_task_, 0) != pdPASS) return ESP_ERR_NO_MEM;

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
#if CONFIG_RO_TEST_HOOKS
        if (xTaskCreatePinnedToCore(&Runtime::diagnostic_task_entry, "ro_test_diag", 4096, this, 2,
                                    &diagnostic_task_, 0) != pdPASS) return ESP_ERR_NO_MEM;
        ESP_LOGW(TAG, "RO TEST HOOKS ENABLED - diagnostic input override is active");
#endif
        return ESP_OK;
    }

private:
    svc::Store store_{};
    svc::AppConfig config_{};
    svc::AdminConfig pending_admin_{};
    bool pending_admin_save_{false};
    PersistentFacts facts_{};
    PersistentFacts queued_facts_{};
    std::array<svc::FilterState,5> filter_states_{};
    hw::Hardware hardware_{};
    Controller controller_;
    svc::TimeService time_{};
    svc::EventLog events_{};
    svc::StatisticsService stats_{};
    svc::FilterService filters_;
    ui::LocalUi ui_;
    std::optional<net::NetworkManager> network_{};
    std::array<std::unique_ptr<hw::FlowMeter>, 3> flow_meters_{};
    std::array<OptionalReading, 3> flow_rates_{};

    QueueHandle_t command_queue_{nullptr};
    QueueHandle_t event_queue_{nullptr};
    QueueHandle_t facts_queue_{nullptr};
    QueueHandle_t stats_queue_{nullptr};
    QueueHandle_t log_queue_{nullptr};
    SemaphoreHandle_t snapshot_mutex_{nullptr};
    TaskHandle_t control_task_{nullptr};
    TaskHandle_t storage_task_{nullptr};
    TaskHandle_t ui_task_{nullptr};
#if CONFIG_RO_TEST_HOOKS
    TaskHandle_t diagnostic_task_{nullptr};
    std::atomic<bool> test_override_{false};
    std::atomic<bool> test_water_{false};
    std::atomic<bool> test_tank_{false};
    std::atomic<bool> test_leak_{false};
#endif
    SystemSnapshot published_snapshot_{};
    std::atomic<uint64_t> reboot_at_ms_{0};
    std::atomic<uint32_t> dropped_storage_work_{0};
    uint64_t ota_validation_deadline_ms_{0};
    bool ota_validated_{false};
    int64_t last_stats_epoch_{0};
    uint64_t last_flow_sample_ms_{0};

    void init_optional_hardware() noexcept {
        for (size_t i = 0; i < flow_meters_.size(); ++i) {
            if (config_.features.flow_enabled[i] && config_.features.flow_pulses_per_l[i] > 0.0F) {
                auto meter = std::make_unique<hw::PcntFlowMeter>(FLOW_PINS[i], config_.features.flow_pulses_per_l[i]);
                if (meter->available()) {
                    flow_meters_[i] = std::move(meter);
                    ESP_LOGI(TAG, "flow channel %u enabled on GPIO%u", static_cast<unsigned>(i),
                             static_cast<unsigned>(FLOW_PINS[i]));
                    continue;
                }
                ESP_LOGW(TAG, "flow channel %u unavailable; continuing without it", static_cast<unsigned>(i));
            }
            flow_meters_[i] = std::make_unique<hw::DisabledFlowMeter>();
        }
    }

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

    void sample_optional_hardware(uint64_t now_ms) noexcept {
        if (now_ms - last_flow_sample_ms_ < 1000) return;
        last_flow_sample_ms_ = now_ms;
        for (size_t i = 0; i < flow_meters_.size(); ++i) {
            flow_rates_[i] = flow_meters_[i] ? flow_meters_[i]->rate_lpm() : OptionalReading{};
        }
    }

    void publish_snapshot() noexcept {
        if (!snapshot_mutex_) return;
        if (xSemaphoreTake(snapshot_mutex_, pdMS_TO_TICKS(20)) == pdTRUE) {
            published_snapshot_ = controller_.snapshot();
            published_snapshot_.rtc_available = time_.rtc_available();
            published_snapshot_.leak_available = config_.hardware.leak_enabled;
            published_snapshot_.feed_flow_lpm = flow_rates_[0];
            published_snapshot_.pure_flow_lpm = flow_rates_[1];
            published_snapshot_.drain_flow_lpm = flow_rates_[2];
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
        const esp_err_t decode_err = store_.apply_config_json(json, next, true);
        if (decode_err != ESP_OK) {
            pending_admin_ = {};
            pending_admin_save_ = false;
            return decode_err;
        }

        esp_err_t save_err = ESP_OK;
        if (pending_admin_save_) {
            next.admin = pending_admin_;
            save_err = store_.save_provisioned(next);
            pending_admin_ = {};
            pending_admin_save_ = false;
        } else {
            save_err = store_.save_config(next);
        }
        if (save_err != ESP_OK) return save_err;

        reboot_at_ms_.store(monotonic_ms() + 1500, std::memory_order_release);
        return ESP_OK;
    }

    esp_err_t set_admin_password(const std::string& password) noexcept {
        svc::AppConfig temporary = config_;
        if (!svc::Security::set_admin_password(temporary, password)) return ESP_ERR_INVALID_ARG;
        // Provisioning stages the verifier in RAM. apply_config() commits the
        // verifier and Wi-Fi/settings in one NVS transaction.
        pending_admin_ = temporary.admin;
        pending_admin_save_ = true;
        return ESP_OK;
    }

    esp_err_t reset_filter(size_t index) noexcept {
        const auto t = time_.now();
        if (!t.valid) return ESP_ERR_INVALID_STATE;
        return filters_.reset(index, t.utc_epoch_s);
    }

    void queue_controller_work(const ControllerEvents& ev, TimeInfo time) noexcept {
        if (has_event(ev)) {
            const EventWork work{ev, time};
            if (xQueueSend(event_queue_, &work, 0) != pdTRUE)
                dropped_storage_work_.fetch_add(1, std::memory_order_relaxed);
        }

        const auto& current = controller_.persistent_facts();
        if (!same_facts(queued_facts_, current)) {
            queued_facts_ = current;
            xQueueOverwrite(facts_queue_, &queued_facts_);
        }
    }

    void queue_stats_sample(const SystemSnapshot& snapshot_value, int64_t epoch) noexcept {
        const StatsWork work{snapshot_value, epoch};
        if (xQueueSend(stats_queue_, &work, 0) != pdTRUE)
            dropped_storage_work_.fetch_add(1, std::memory_order_relaxed);
    }

    void queue_log(int64_t epoch, const char* message) noexcept {
        LogWork work{};
        work.epoch = epoch;
        std::snprintf(work.message, sizeof(work.message), "%s", message ? message : "");
        if (xQueueSend(log_queue_, &work, 0) != pdTRUE)
            dropped_storage_work_.fetch_add(1, std::memory_order_relaxed);
    }

    void process_event_work(const EventWork& work, bool& checkpoint_requested) noexcept {
        const auto& ev = work.events;
        const int64_t epoch = work.time.valid ? work.time.utc_epoch_s : 0;
        if (ev.state_changed) {
            char message[96];
            std::snprintf(message, sizeof(message), "state %s -> %s",
                          to_string(ev.previous_state), to_string(ev.new_state));
            events_.append(epoch, message);
            stats_.on_transition(ev.previous_state, ev.new_state, local_calendar_epoch(epoch));
            if (ev.new_state == State::Standby) checkpoint_requested = true;
        }
        if (ev.error_latched) {
            char message[96];
            std::snprintf(message, sizeof(message), "error latched: %s", to_string(ev.error));
            events_.append(epoch, message);
        }
        if (ev.error_cleared) events_.append(epoch, "error reset");
        if (ev.membrane_flush_completed) events_.append(epoch, "membrane flush completed");
    }

    void storage_loop() noexcept {
        uint64_t last_checkpoint_ms = monotonic_ms();
        for (;;) {
            bool did_work = false;
            bool checkpoint_requested = false;

            PersistentFacts new_facts{};
            while (xQueueReceive(facts_queue_, &new_facts, 0) == pdTRUE) {
                did_work = true;
                if (!same_facts(facts_, new_facts)) {
                    const esp_err_t err = store_.save_facts(new_facts);
                    if (err == ESP_OK) facts_ = new_facts;
                    else ESP_LOGE(TAG, "unable to persist controller facts: %s", esp_err_to_name(err));
                }
            }

            EventWork event_work{};
            while (xQueueReceive(event_queue_, &event_work, 0) == pdTRUE) {
                did_work = true;
                process_event_work(event_work, checkpoint_requested);
            }

            StatsWork stats_work{};
            while (xQueueReceive(stats_queue_, &stats_work, 0) == pdTRUE) {
                did_work = true;
                stats_.observe_second(stats_work.snapshot, stats_work.epoch);
            }

            LogWork log_work{};
            while (xQueueReceive(log_queue_, &log_work, 0) == pdTRUE) {
                did_work = true;
                events_.append(log_work.epoch, log_work.message);
            }

            const uint64_t now_ms = monotonic_ms();
            if (checkpoint_requested || now_ms - last_checkpoint_ms >= STATS_CHECKPOINT_MS) {
                const esp_err_t err = stats_.checkpoint();
                if (err != ESP_OK) ESP_LOGW(TAG, "statistics checkpoint failed: %s", esp_err_to_name(err));
                last_checkpoint_ms = now_ms;
            }

            const uint32_t dropped = dropped_storage_work_.exchange(0, std::memory_order_relaxed);
            if (dropped != 0) ESP_LOGW(TAG, "storage queue dropped %u non-critical work items", dropped);

            if (!did_work) vTaskDelay(pdMS_TO_TICKS(20));
        }
    }

    void validate_ota_if_ready(uint64_t now_ms) noexcept {
        if (ota_validated_ || now_ms < ota_validation_deadline_ms_) return;
        const esp_partition_t* running = esp_ota_get_running_partition();
        esp_ota_img_states_t state{};
        if (running && esp_ota_get_state_partition(running, &state) == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY) {
            if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
                ESP_LOGI(TAG, "OTA image marked valid after stable control runtime");
                const auto t = time_.now();
                queue_log(t.valid ? t.utc_epoch_s : 0, "OTA image validated");
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

    Inputs effective_inputs() const noexcept {
#if CONFIG_RO_TEST_HOOKS
        if (test_override_.load(std::memory_order_acquire)) {
            return {
                test_water_.load(std::memory_order_relaxed),
                test_tank_.load(std::memory_order_relaxed),
                test_leak_.load(std::memory_order_relaxed),
            };
        }
#endif
        return hardware_.read_inputs();
    }

    void control_loop() noexcept {
        esp_task_wdt_add(nullptr);
        TickType_t wake = xTaskGetTickCount();
        for (;;) {
            const uint64_t now_ms = monotonic_ms();
            const Inputs inputs = effective_inputs();
            const TimeInfo current_time = time_.now();

            Command cmd{};
            while (xQueueReceive(command_queue_, &cmd, 0) == pdTRUE) {
                const ControllerEvents ev = controller_.command(now_ms, inputs, cmd, current_time);
                queue_controller_work(ev, current_time);
            }

            const ControllerEvents ev = controller_.tick(now_ms, inputs, current_time);
            queue_controller_work(ev, current_time);
            hardware_.apply_outputs(controller_.snapshot().outputs);
            sample_optional_hardware(now_ms);
            publish_snapshot();

            if (current_time.valid && current_time.utc_epoch_s != last_stats_epoch_) {
                queue_stats_sample(published_snapshot_, local_calendar_epoch(current_time.utc_epoch_s));
                last_stats_epoch_ = current_time.utc_epoch_s;
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

#if CONFIG_RO_TEST_HOOKS
    void print_diagnostic_snapshot() noexcept {
        const auto s = snapshot();
        std::printf("RO_TEST {\"state\":\"%s\",\"error\":\"%s\",\"water\":%s,\"tank\":%s,\"leak\":%s,\"inlet\":%s,\"pump\":%s,\"flush\":%s}\n",
                    to_string(s.state), to_string(s.error),
                    s.inputs.water_available ? "true" : "false",
                    s.inputs.tank_full ? "true" : "false",
                    s.inputs.leak_detected ? "true" : "false",
                    s.outputs.inlet ? "true" : "false",
                    s.outputs.pump ? "true" : "false",
                    s.outputs.flush ? "true" : "false");
        std::fflush(stdout);
    }

    void diagnostic_loop() noexcept {
        char line[160]{};
        for (;;) {
            if (!std::fgets(line, sizeof(line), stdin)) {
                clearerr(stdin);
                vTaskDelay(pdMS_TO_TICKS(50));
                continue;
            }
            while (std::strlen(line) > 0 && (line[std::strlen(line) - 1] == '\n' || line[std::strlen(line) - 1] == '\r'))
                line[std::strlen(line) - 1] = '\0';

            if (std::strcmp(line, "snapshot") == 0) {
                print_diagnostic_snapshot();
                continue;
            }
            if (std::strcmp(line, "simulate:off") == 0) {
                test_override_.store(false, std::memory_order_release);
                std::puts("RO_TEST OK simulate:off");
                std::fflush(stdout);
                continue;
            }
            int water = 0, tank = 0, leak = 0;
            if (std::sscanf(line, "simulate:water=%d,tank=%d,leak=%d", &water, &tank, &leak) >= 2) {
                test_water_.store(water != 0, std::memory_order_relaxed);
                test_tank_.store(tank != 0, std::memory_order_relaxed);
                test_leak_.store(leak != 0, std::memory_order_relaxed);
                test_override_.store(true, std::memory_order_release);
                std::puts("RO_TEST OK simulate");
                std::fflush(stdout);
                continue;
            }
            if (std::strcmp(line, "network:down") == 0) {
                esp_wifi_stop();
                std::puts("RO_TEST OK network:down");
                std::fflush(stdout);
                continue;
            }
            if (std::strcmp(line, "network:up") == 0) {
                esp_wifi_start();
                esp_wifi_connect();
                std::puts("RO_TEST OK network:up");
                std::fflush(stdout);
                continue;
            }
            if (std::strcmp(line, "command:flush") == 0) {
                enqueue({CommandType::StartManualFlush, CommandSource::Local});
                std::puts("RO_TEST OK command:flush");
                std::fflush(stdout);
                continue;
            }
            if (std::strcmp(line, "command:reset_error") == 0) {
                enqueue({CommandType::ResetError, CommandSource::Local});
                std::puts("RO_TEST OK command:reset_error");
                std::fflush(stdout);
                continue;
            }
            std::puts("RO_TEST ERR unknown-command");
            std::fflush(stdout);
        }
    }
#endif

    static void control_task_entry(void* arg) {
        static_cast<Runtime*>(arg)->control_loop();
    }

    static void storage_task_entry(void* arg) {
        static_cast<Runtime*>(arg)->storage_loop();
    }

    static void ui_task_entry(void* arg) {
        static_cast<Runtime*>(arg)->ui_loop();
    }

#if CONFIG_RO_TEST_HOOKS
    static void diagnostic_task_entry(void* arg) {
        static_cast<Runtime*>(arg)->diagnostic_loop();
    }
#endif
};

Runtime runtime;
}

void start() {
    const esp_err_t err = runtime.init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "fatal initialization error: %s", esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_restart();
    }
}

} // namespace ro::app
