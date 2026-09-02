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
constexpr std::array<gpio_num_t,3> FLOW_PINS{GPIO_NUM_4, GPIO_NUM_5, GPIO_NUM_16};

uint64_t monotonic_ms() noexcept { return static_cast<uint64_t>(esp_timer_get_time() / 1000); }

int64_t local_calendar_epoch(int64_t utc_epoch) noexcept {
    if (utc_epoch <= 0) return 0;
    const time_t utc = static_cast<time_t>(utc_epoch);
    std::tm local{};
    localtime_r(&utc, &local);
    return static_cast<int64_t>(timegm(&local));
}

void preload_safe_output_latches(const hw::HardwareConfig& cfg) noexcept {
    gpio_set_level(GPIO_NUM_13, cfg.inlet_active_high ? 0 : 1);
    gpio_set_level(GPIO_NUM_14, cfg.pump_active_high ? 0 : 1);
    gpio_set_level(GPIO_NUM_15, cfg.flush_active_high ? 0 : 1);
}

bool same_facts(const PersistentFacts& a, const PersistentFacts& b) noexcept {
    return a.latched_error == b.latched_error && a.last_membrane_flush_utc_s == b.last_membrane_flush_utc_s;
}

bool has_event(const ControllerEvents& e) noexcept {
    return e.state_changed || e.error_latched || e.error_cleared || e.membrane_flush_completed ||
           e.production_started || e.tank_became_full || e.manual_flush_started;
}

struct EventWork { ControllerEvents events{}; TimeInfo time{}; };
struct StatsWork { SystemSnapshot snapshot{}; int64_t local_epoch{0}; };
struct LogWork { int64_t epoch{0}; char message[96]{}; };
static_assert(std::is_trivially_copyable_v<EventWork> && std::is_trivially_copyable_v<StatsWork> &&
              std::is_trivially_copyable_v<LogWork> && std::is_trivially_copyable_v<PersistentFacts>);

class Runtime {
public:
    Runtime() noexcept : config_(svc::AppConfig::defaults()), controller_(config_.controller),
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
        const esp_err_t restore = stats_.restore_durable_checkpoint();
        if (restore != ESP_OK) ESP_LOGW(TAG, "statistics checkpoint ignored: %s", esp_err_to_name(restore));
        init_optional_hardware();

        command_queue_ = xQueueCreate(16, sizeof(Command));
        event_queue_ = xQueueCreate(32, sizeof(EventWork));
        facts_queue_ = xQueueCreate(1, sizeof(PersistentFacts));
        stats_queue_ = xQueueCreate(16, sizeof(StatsWork));
        log_queue_ = xQueueCreate(16, sizeof(LogWork));
        snapshot_mutex_ = xSemaphoreCreateMutex();
        if (!command_queue_ || !event_queue_ || !facts_queue_ || !stats_queue_ || !log_queue_ || !snapshot_mutex_) return ESP_ERR_NO_MEM;

        if (xTaskCreatePinnedToCore(&Runtime::storage_task_entry, "ro_storage", 6144, this, 3, &storage_task_, 0) != pdPASS) return ESP_ERR_NO_MEM;

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
        if (xTaskCreatePinnedToCore(&Runtime::control_task_entry, "ro_control", 8192, this, 12, &control_task_, 1) != pdPASS) return ESP_ERR_NO_MEM;
        if (xTaskCreatePinnedToCore(&Runtime::ui_task_entry, "ro_ui", 6144, this, 5, &ui_task_, 0) != pdPASS) return ESP_ERR_NO_MEM;
#if CONFIG_RO_TEST_HOOKS
        if (xTaskCreatePinnedToCore(&Runtime::diagnostic_task_entry, "ro_test_diag", 4096, this, 2, &diagnostic_task_, 0) != pdPASS) return ESP_ERR_NO_MEM;
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
    std::array<std::unique_ptr<hw::FlowMeter>,3> flow_meters_{};
    std::array<OptionalReading,3> flow_rates_{};

    QueueHandle_t command_queue_{nullptr}, event_queue_{nullptr}, facts_queue_{nullptr}, stats_queue_{nullptr}, log_queue_{nullptr};
    SemaphoreHandle_t snapshot_mutex_{nullptr};
    TaskHandle_t control_task_{nullptr}, storage_task_{nullptr}, ui_task_{nullptr};
#if CONFIG_RO_TEST_HOOKS
    TaskHandle_t diagnostic_task_{nullptr};
    std::atomic<bool> test_override_{false}, test_water_{false}, test_tank_{false}, test_leak_{false};
#endif
    SystemSnapshot published_snapshot_{};
    std::atomic<uint64_t> reboot_at_ms_{0};
    std::atomic<uint32_t> dropped_storage_work_{0};
    uint64_t ota_validation_deadline_ms_{0};
    bool ota_validated_{false};
    int64_t last_stats_epoch_{0};
    uint64_t last_flow_sample_ms_{0};

    void init_optional_hardware() noexcept {
        for (size_t i=0; i<flow_meters_.size(); ++i) {
            if (config_.features.flow_enabled[i] && config_.features.flow_pulses_per_l[i] > 0.0F) {
                auto meter = std::make_unique<hw::PcntFlowMeter>(FLOW_PINS[i], config_.features.flow_pulses_per_l[i]);
                if (meter->available()) { flow_meters_[i] = std::move(meter); continue; }
                ESP_LOGW(TAG, "flow channel %u unavailable", static_cast<unsigned>(i));
            }
            flow_meters_[i] = std::make_unique<hw::DisabledFlowMeter>();
        }
    }

    static SystemSnapshot hook_snapshot(void* c) noexcept { return static_cast<Runtime*>(c)->snapshot(); }
    static bool hook_enqueue(void* c, const Command& x) noexcept { return static_cast<Runtime*>(c)->enqueue(x); }
    static bool hook_ota_hold(void* c, bool x) noexcept { return static_cast<Runtime*>(c)->set_ota_hold(x); }
    static esp_err_t hook_apply_config(void* c, const std::string& x) noexcept { return static_cast<Runtime*>(c)->apply_config(x); }
    static esp_err_t hook_set_admin_password(void* c, const std::string& x) noexcept { return static_cast<Runtime*>(c)->set_admin_password(x); }
    static esp_err_t hook_reset_filter(void* c, size_t x) noexcept { return static_cast<Runtime*>(c)->reset_filter(x); }

    SystemSnapshot snapshot() noexcept {
        SystemSnapshot copy{};
        if (!snapshot_mutex_) return published_snapshot_;
        if (xSemaphoreTake(snapshot_mutex_, pdMS_TO_TICKS(100)) == pdTRUE) { copy = published_snapshot_; xSemaphoreGive(snapshot_mutex_); }
        return copy;
    }

    void sample_optional_hardware(uint64_t now_ms) noexcept {
        if (now_ms - last_flow_sample_ms_ < 1000) return;
        last_flow_sample_ms_ = now_ms;
        for (size_t i=0; i<flow_meters_.size(); ++i) flow_rates_[i] = flow_meters_[i] ? flow_meters_[i]->rate_lpm() : OptionalReading{};
    }

    void publish_snapshot() noexcept {
        if (xSemaphoreTake(snapshot_mutex_, pdMS_TO_TICKS(20)) != pdTRUE) return;
        published_snapshot_ = controller_.snapshot();
        published_snapshot_.rtc_available = time_.rtc_available();
        published_snapshot_.leak_available = config_.hardware.leak_enabled;
        published_snapshot_.feed_flow_lpm = flow_rates_[0];
        published_snapshot_.pure_flow_lpm = flow_rates_[1];
        published_snapshot_.drain_flow_lpm = flow_rates_[2];
        xSemaphoreGive(snapshot_mutex_);
    }

    bool enqueue(const Command& cmd) noexcept { return command_queue_ && xQueueSend(command_queue_, &cmd, 0) == pdTRUE; }

    bool set_ota_hold(bool enabled) noexcept {
        const Command cmd{enabled ? CommandType::EnterOtaHold : CommandType::ExitOtaHold, CommandSource::Internal};
        if (!enqueue(cmd)) return false;
        const uint64_t deadline = monotonic_ms() + 1000;
        while (monotonic_ms() < deadline) {
            const auto s = snapshot();
            if (enabled && s.state == State::OtaHold && !s.outputs.inlet && !s.outputs.pump && !s.outputs.flush) return true;
            if (!enabled && s.state != State::OtaHold) return true;
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        return false;
    }

    esp_err_t apply_config(const std::string& json) noexcept {
        svc::AppConfig next = config_;
        const esp_err_t decode = store_.apply_config_json(json, next, true);
        if (decode != ESP_OK) { pending_admin_ = {}; pending_admin_save_ = false; return decode; }
        esp_err_t save = ESP_OK;
        if (pending_admin_save_) {
            next.admin = pending_admin_;
            save = store_.save_provisioned(next);
            pending_admin_ = {};
            pending_admin_save_ = false;
        } else save = store_.save_config(next);
        if (save != ESP_OK) return save;
        reboot_at_ms_.store(monotonic_ms() + 1500, std::memory_order_release);
        return ESP_OK;
    }

    esp_err_t set_admin_password(const std::string& password) noexcept {
        svc::AppConfig temp = config_;
        if (!svc::Security::set_admin_password(temp, password)) return ESP_ERR_INVALID_ARG;
        pending_admin_ = temp.admin;
        pending_admin_save_ = true;
        return ESP_OK;
    }

    esp_err_t reset_filter(size_t index) noexcept {
        const auto t = time_.now();
        return t.valid ? filters_.reset(index, t.utc_epoch_s) : ESP_ERR_INVALID_STATE;
    }

    void queue_controller_work(const ControllerEvents& ev, TimeInfo time) noexcept {
        if (has_event(ev)) {
            const EventWork w{ev,time};
            if (xQueueSend(event_queue_, &w, 0) != pdTRUE) dropped_storage_work_.fetch_add(1, std::memory_order_relaxed);
        }
        const auto& current = controller_.persistent_facts();
        if (!same_facts(queued_facts_, current)) { queued_facts_ = current; xQueueOverwrite(facts_queue_, &queued_facts_); }
    }

    void queue_stats_sample(const SystemSnapshot& s, int64_t local_epoch) noexcept {
        const StatsWork w{s,local_epoch};
        if (xQueueSend(stats_queue_, &w, 0) != pdTRUE) dropped_storage_work_.fetch_add(1, std::memory_order_relaxed);
    }

    void queue_log(int64_t epoch, const char* text) noexcept {
        LogWork w{}; w.epoch = epoch; std::snprintf(w.message, sizeof(w.message), "%s", text ? text : "");
        if (xQueueSend(log_queue_, &w, 0) != pdTRUE) dropped_storage_work_.fetch_add(1, std::memory_order_relaxed);
    }

    void process_event_work(const EventWork& w, bool& checkpoint_requested) noexcept {
        const auto& ev = w.events;
        const int64_t utc = w.time.valid ? w.time.utc_epoch_s : 0;
        const int64_t local_epoch = local_calendar_epoch(utc);
        if (ev.state_changed) {
            char message[96];
            std::snprintf(message, sizeof(message), "state %s -> %s", to_string(ev.previous_state), to_string(ev.new_state));
            events_.append(utc, message);
            stats_.on_transition(ev.previous_state, ev.new_state, local_epoch);
            if (ev.new_state == State::Standby) checkpoint_requested = true;
        }
        if (ev.manual_flush_started) stats_.on_manual_flush(local_epoch, ev.previous_state == State::Standby);
        if (ev.error_latched) { char m[96]; std::snprintf(m,sizeof(m),"error latched: %s",to_string(ev.error)); events_.append(utc,m); }
        if (ev.error_cleared) events_.append(utc, "error reset");
        if (ev.membrane_flush_completed) events_.append(utc, "membrane flush completed");
    }

    void storage_loop() noexcept {
        uint64_t last_checkpoint_ms = monotonic_ms();
        for (;;) {
            bool did_work=false, checkpoint_requested=false;
            PersistentFacts f{};
            while (xQueueReceive(facts_queue_, &f, 0) == pdTRUE) {
                did_work=true;
                if (!same_facts(facts_,f)) { const auto e=store_.save_facts(f); if (e==ESP_OK) facts_=f; else ESP_LOGE(TAG,"persist facts: %s",esp_err_to_name(e)); }
            }
            EventWork ew{}; while (xQueueReceive(event_queue_, &ew, 0) == pdTRUE) { did_work=true; process_event_work(ew,checkpoint_requested); }
            StatsWork sw{}; while (xQueueReceive(stats_queue_, &sw, 0) == pdTRUE) { did_work=true; stats_.observe_second(sw.snapshot,sw.local_epoch); }
            LogWork lw{}; while (xQueueReceive(log_queue_, &lw, 0) == pdTRUE) { did_work=true; events_.append(lw.epoch,lw.message); }
            const uint64_t now=monotonic_ms();
            if (checkpoint_requested || now-last_checkpoint_ms >= STATS_CHECKPOINT_MS) {
                const auto e=stats_.persist_durable_checkpoint();
                if (e!=ESP_OK) ESP_LOGW(TAG,"statistics checkpoint failed: %s",esp_err_to_name(e));
                last_checkpoint_ms=now;
            }
            const uint32_t dropped=dropped_storage_work_.exchange(0,std::memory_order_relaxed);
            if (dropped) ESP_LOGW(TAG,"storage queue dropped %u non-critical work items",dropped);
            if (!did_work) vTaskDelay(pdMS_TO_TICKS(20));
        }
    }

    void validate_ota_if_ready(uint64_t now_ms) noexcept {
        if (ota_validated_ || now_ms < ota_validation_deadline_ms_) return;
        const esp_partition_t* running=esp_ota_get_running_partition();
        esp_ota_img_states_t state{};
        if (running && esp_ota_get_state_partition(running,&state)==ESP_OK && state==ESP_OTA_IMG_PENDING_VERIFY) {
            const auto s=snapshot();
            const bool outputs_safe = !s.outputs.inlet && !s.outputs.pump && !s.outputs.flush;
            const bool control_live = control_task_ != nullptr;
            if (control_live && (outputs_safe || s.state != State::OtaHold) && esp_ota_mark_app_valid_cancel_rollback()==ESP_OK) {
                ESP_LOGI(TAG,"OTA image marked valid after stable control runtime");
                const auto t=time_.now(); queue_log(t.valid?t.utc_epoch_s:0,"OTA image validated");
            } else return;
        }
        ota_validated_=true;
    }

    void maybe_reboot(uint64_t now_ms) noexcept {
        const uint64_t at=reboot_at_ms_.load(std::memory_order_acquire);
        if (!at || now_ms<at) return;
        hardware_.force_all_off(); vTaskDelay(pdMS_TO_TICKS(100)); esp_restart();
    }

    Inputs effective_inputs() const noexcept {
#if CONFIG_RO_TEST_HOOKS
        if (test_override_.load(std::memory_order_acquire)) return {test_water_.load(),test_tank_.load(),test_leak_.load()};
#endif
        return hardware_.read_inputs();
    }

    void control_loop() noexcept {
        esp_task_wdt_add(nullptr);
        TickType_t wake=xTaskGetTickCount();
        for (;;) {
            const uint64_t now=monotonic_ms();
            const Inputs in=effective_inputs();
            const TimeInfo ti=time_.now();
            Command cmd{};
            while (xQueueReceive(command_queue_,&cmd,0)==pdTRUE) {
                ControllerEvents ev=controller_.command(now,in,cmd,ti);
                if (cmd.type==CommandType::StartManualFlush && ev.state_changed && ev.new_state==State::StandbyFlush) ev.manual_flush_started=true;
                queue_controller_work(ev,ti);
            }
            const ControllerEvents ev=controller_.tick(now,in,ti);
            queue_controller_work(ev,ti);
            hardware_.apply_outputs(controller_.snapshot().outputs);
            sample_optional_hardware(now);
            publish_snapshot();
            if (ti.valid && ti.utc_epoch_s!=last_stats_epoch_) { queue_stats_sample(published_snapshot_,local_calendar_epoch(ti.utc_epoch_s)); last_stats_epoch_=ti.utc_epoch_s; }
            validate_ota_if_ready(now);
            maybe_reboot(now);
            esp_task_wdt_reset();
            vTaskDelayUntil(&wake,CONTROL_PERIOD);
        }
    }

    void ui_loop() noexcept {
        for (;;) {
            const uint64_t now=monotonic_ms();
            const auto s=snapshot();
            if (auto cmd=ui_.poll_command(now,s)) enqueue(*cmd);
            ui_.update(now,s,time_.now().valid,time_.source_name());
            if (network_) network_->publish_periodic(now);
            vTaskDelay(UI_PERIOD);
        }
    }

#if CONFIG_RO_TEST_HOOKS
    void print_diagnostic_snapshot() noexcept {
        const auto s=snapshot();
        std::printf("RO_TEST {\"state\":\"%s\",\"error\":\"%s\",\"water\":%s,\"tank\":%s,\"leak\":%s,\"inlet\":%s,\"pump\":%s,\"flush\":%s}\n",
            to_string(s.state),to_string(s.error),s.inputs.water_available?"true":"false",s.inputs.tank_full?"true":"false",s.inputs.leak_detected?"true":"false",
            s.outputs.inlet?"true":"false",s.outputs.pump?"true":"false",s.outputs.flush?"true":"false");
        std::fflush(stdout);
    }
    void diagnostic_loop() noexcept {
        char line[160]{};
        for (;;) {
            if (!std::fgets(line,sizeof(line),stdin)) { clearerr(stdin); vTaskDelay(pdMS_TO_TICKS(50)); continue; }
            while (std::strlen(line)>0 && (line[std::strlen(line)-1]=='\n'||line[std::strlen(line)-1]=='\r')) line[std::strlen(line)-1]='\0';
            if (!std::strcmp(line,"snapshot")) { print_diagnostic_snapshot(); continue; }
            if (!std::strcmp(line,"simulate:off")) { test_override_.store(false); std::puts("RO_TEST OK simulate:off"); std::fflush(stdout); continue; }
            int water=0,tank=0,leak=0;
            if (std::sscanf(line,"simulate:water=%d,tank=%d,leak=%d",&water,&tank,&leak)>=2) {
                test_water_.store(water!=0); test_tank_.store(tank!=0); test_leak_.store(leak!=0); test_override_.store(true);
                std::puts("RO_TEST OK simulate"); std::fflush(stdout); continue;
            }
            if (!std::strcmp(line,"network:down")) { esp_wifi_stop(); std::puts("RO_TEST OK network:down"); std::fflush(stdout); continue; }
            if (!std::strcmp(line,"network:up")) { esp_wifi_start(); esp_wifi_connect(); std::puts("RO_TEST OK network:up"); std::fflush(stdout); continue; }
            if (!std::strcmp(line,"command:flush")) { enqueue({CommandType::StartManualFlush,CommandSource::Local}); std::puts("RO_TEST OK command:flush"); std::fflush(stdout); continue; }
            if (!std::strcmp(line,"command:reset_error")) { enqueue({CommandType::ResetError,CommandSource::Local}); std::puts("RO_TEST OK command:reset_error"); std::fflush(stdout); continue; }
            std::puts("RO_TEST ERR unknown-command"); std::fflush(stdout);
        }
    }
#endif

    static void control_task_entry(void* p) { static_cast<Runtime*>(p)->control_loop(); }
    static void storage_task_entry(void* p) { static_cast<Runtime*>(p)->storage_loop(); }
    static void ui_task_entry(void* p) { static_cast<Runtime*>(p)->ui_loop(); }
#if CONFIG_RO_TEST_HOOKS
    static void diagnostic_task_entry(void* p) { static_cast<Runtime*>(p)->diagnostic_loop(); }
#endif
};

Runtime runtime;
}

void start() {
    const esp_err_t err=runtime.init();
    if (err!=ESP_OK) { ESP_LOGE(TAG,"fatal initialization error: %s",esp_err_to_name(err)); vTaskDelay(pdMS_TO_TICKS(2000)); esp_restart(); }
}
} // namespace ro::app
