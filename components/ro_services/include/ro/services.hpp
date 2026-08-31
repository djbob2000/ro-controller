#pragma once

#include "ro/controller.hpp"
#include "ro/hardware.hpp"
#include "driver/i2c_master.h"
#include "esp_err.h"
#include <array>
#include <cstdint>
#include <optional>
#include <string>

namespace ro::svc {

struct WifiConfig {
    std::string ssid;
    std::string password;
};

struct MqttConfig {
    bool enabled{false};
    bool tls{false};
    std::string host;
    uint16_t port{1883};
    std::string username;
    std::string password;
    std::string base_topic{"ro-controller"};
    std::string discovery_prefix{"homeassistant"};
};

struct AdminConfig {
    bool configured{false};
    std::array<uint8_t,16> salt{};
    std::array<uint8_t,32> hash{};
};

enum class VolumeSource : uint8_t { Feed, Pure, Drain };
struct FilterConfig {
    std::string name;
    uint32_t calendar_days{180};
    float volume_limit_l{5000.0F};
    VolumeSource volume_source{VolumeSource::Feed};
};
struct FilterState {
    int64_t installation_utc_s{0};
    uint64_t consumed_ml{0};
};

struct FeatureConfig {
    std::array<bool,3> flow_enabled{false,false,false};
    std::array<float,3> flow_pulses_per_l{1920.0F,1920.0F,1920.0F};
    bool tds_feed_enabled{false};
    bool tds_pure_enabled{false};
    bool recovery_enabled{false};
    float target_recovery_percent{20.0F};
};

struct AppConfig {
    uint32_t version{1};
    Settings controller{};
    hw::HardwareConfig hardware{};
    WifiConfig wifi{};
    MqttConfig mqtt{};
    AdminConfig admin{};
    std::string timezone_name{"Etc/UTC"};
    std::string timezone_posix{"UTC0"};
    int16_t fallback_utc_offset_minutes{0};
    std::array<FilterConfig,5> filters{};
    FeatureConfig features{};

    static AppConfig defaults();
};

class Store {
public:
    esp_err_t init() noexcept;
    esp_err_t load(AppConfig& cfg, PersistentFacts& facts, std::array<FilterState,5>& filters) noexcept;
    esp_err_t save_config(const AppConfig& cfg) noexcept;
    esp_err_t load_admin(AdminConfig& admin) noexcept;
    esp_err_t save_admin(const AdminConfig& admin) noexcept;
    esp_err_t save_facts(const PersistentFacts& facts) noexcept;
    esp_err_t save_filter_state(size_t index, const FilterState& state) noexcept;
    esp_err_t load_tls_identity(std::string& cert_pem, std::string& key_pem) noexcept;
    esp_err_t save_tls_identity(const std::string& cert_pem, const std::string& key_pem) noexcept;
    esp_err_t clear_admin() noexcept;
    esp_err_t factory_reset() noexcept;

    std::string config_json(const AppConfig& cfg, bool include_secrets) const;
    esp_err_t apply_config_json(const std::string& json, AppConfig& cfg, bool allow_secrets) const noexcept;

private:
    esp_err_t mount_spiffs() noexcept;
};

class Security {
public:
    static bool set_admin_password(AppConfig& cfg, const std::string& password) noexcept;
    static bool verify_admin_password(const AppConfig& cfg, const std::string& password) noexcept;
    static std::string random_hex(size_t bytes);
    static bool generate_tls_identity(std::string& cert_pem, std::string& key_pem) noexcept;
    static std::string encrypt_backup(const std::string& plaintext, const std::string& passphrase);
    static std::optional<std::string> decrypt_backup(const std::string& envelope, const std::string& passphrase);
};

class TimeService {
public:
    esp_err_t init(i2c_master_bus_handle_t bus, const AppConfig& cfg) noexcept;
    void apply_timezone(const AppConfig& cfg) noexcept;
    void start_sntp() noexcept;
    TimeInfo now() noexcept;
    bool rtc_available() const noexcept { return rtc_dev_ != nullptr; }
    bool ntp_synced() const noexcept { return ntp_synced_; }
    const char* source_name() const noexcept;
    static std::string posix_for_browser_timezone(const std::string& iana, int offset_minutes);

private:
    i2c_master_dev_handle_t rtc_dev_{nullptr};
    bool ntp_synced_{false};
    bool sntp_started_{false};
    bool rtc_time_loaded_{false};
    std::string timezone_posix_{"UTC0"};
    esp_err_t read_rtc(int64_t& epoch) noexcept;
    esp_err_t write_rtc(int64_t epoch) noexcept;
    static void sntp_sync_cb(struct timeval* tv);
    static TimeService* instance_;
};

class EventLog {
public:
    esp_err_t append(int64_t epoch, const std::string& event) noexcept;
    std::string tail(size_t max_lines = 100) const;
    esp_err_t clear() noexcept;
private:
    void compact_if_needed() noexcept;
};

struct DailyStats {
    int64_t day_key{0};
    uint64_t pump_runtime_s{0};
    uint32_t production_cycles{0};
    uint32_t final_flushes{0};
    uint32_t standby_flushes{0};
    uint32_t manual_flushes{0};
    double feed_l{0};
    double pure_l{0};
    double drain_l{0};
};

class StatisticsService {
public:
    esp_err_t init() noexcept;
    void observe_second(const SystemSnapshot& snapshot, int64_t epoch) noexcept;
    void on_transition(State from, State to, int64_t epoch) noexcept;
    std::string current_json() const;
    std::string history_tail(size_t max_lines = 90) const;
    esp_err_t checkpoint() noexcept;
private:
    DailyStats current_{};
    int64_t last_observed_epoch_{0};
    void rollover_if_needed(int64_t epoch) noexcept;
    esp_err_t append_current() noexcept;
};

class FilterService {
public:
    FilterService(Store& store, AppConfig& cfg, std::array<FilterState,5>& states) noexcept
        : store_(store), cfg_(cfg), states_(states) {}
    esp_err_t reset(size_t index, int64_t now_epoch) noexcept;
    void add_volume(VolumeSource source, uint64_t ml) noexcept;
    std::string status_json(int64_t now_epoch, bool time_valid) const;
private:
    Store& store_;
    AppConfig& cfg_;
    std::array<FilterState,5>& states_;
};

} // namespace ro::svc
