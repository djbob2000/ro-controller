#include "ro/services.hpp"

#include "cJSON.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_spiffs.h"
#include "esp_sntp.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "mbedtls/base64.h"
#include "mbedtls/gcm.h"
#include "mbedtls/md.h"
#include "mbedtls/pk.h"
#include "mbedtls/pkcs5.h"
#include "mbedtls/rsa.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/x509write.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <sys/time.h>
#include <vector>

namespace ro::svc {
namespace {
constexpr char TAG[] = "ro_services";
constexpr char NVS_NS[] = "ro";
constexpr char SPIFFS_BASE[] = "/spiffs";
constexpr char EVENT_PATH[] = "/spiffs/events.log";
constexpr char STATS_PATH[] = "/spiffs/stats.log";
constexpr size_t EVENT_MAX_BYTES = 128 * 1024;
constexpr size_t EVENT_KEEP_BYTES = 64 * 1024;
constexpr uint32_t PASSWORD_PBKDF2_ROUNDS = 120'000;
constexpr uint32_t BACKUP_PBKDF2_ROUNDS = 150'000;

int esp_rng(void*, unsigned char* output, size_t len) {
    esp_fill_random(output, len);
    return 0;
}

bool ct_equal(const uint8_t* a, const uint8_t* b, size_t n) noexcept {
    uint8_t diff = 0;
    for (size_t i = 0; i < n; ++i) diff |= static_cast<uint8_t>(a[i] ^ b[i]);
    return diff == 0;
}

bool pbkdf2_sha256(const std::string& password, const uint8_t* salt, size_t salt_len,
                   uint32_t rounds, uint8_t* out, size_t out_len) noexcept {
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!info || mbedtls_md_setup(&ctx, info, 1) != 0) {
        mbedtls_md_free(&ctx);
        return false;
    }
    const int rc = mbedtls_pkcs5_pbkdf2_hmac(
        &ctx,
        reinterpret_cast<const unsigned char*>(password.data()), password.size(),
        salt, salt_len, rounds, out_len, out);
    mbedtls_md_free(&ctx);
    return rc == 0;
}

std::string b64_encode(const uint8_t* data, size_t len) {
    size_t need = 0;
    mbedtls_base64_encode(nullptr, 0, &need, data, len);
    std::string out;
    out.resize(need);
    size_t written = 0;
    if (mbedtls_base64_encode(reinterpret_cast<unsigned char*>(out.data()), out.size(), &written, data, len) != 0) return {};
    out.resize(written);
    return out;
}

std::optional<std::vector<uint8_t>> b64_decode(const std::string& text) {
    size_t need = 0;
    const auto* src = reinterpret_cast<const unsigned char*>(text.data());
    const int probe = mbedtls_base64_decode(nullptr, 0, &need, src, text.size());
    if (probe != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL && probe != 0) return std::nullopt;
    std::vector<uint8_t> out(need);
    size_t written = 0;
    if (mbedtls_base64_decode(out.data(), out.size(), &written, src, text.size()) != 0) return std::nullopt;
    out.resize(written);
    return out;
}

esp_err_t open_nvs(nvs_open_mode_t mode, nvs_handle_t& h) noexcept {
    return nvs_open(NVS_NS, mode, &h);
}

esp_err_t save_string(nvs_handle_t h, const char* key, const std::string& value) noexcept {
    return nvs_set_str(h, key, value.c_str());
}

esp_err_t load_string(nvs_handle_t h, const char* key, std::string& value) noexcept {
    size_t size = 0;
    esp_err_t err = nvs_get_str(h, key, nullptr, &size);
    if (err != ESP_OK) return err;
    std::vector<char> buf(size);
    err = nvs_get_str(h, key, buf.data(), &size);
    if (err == ESP_OK) value.assign(buf.data());
    return err;
}

void add_bool(cJSON* o, const char* key, bool value) { cJSON_AddBoolToObject(o, key, value); }
void add_num(cJSON* o, const char* key, double value) { cJSON_AddNumberToObject(o, key, value); }
void add_str(cJSON* o, const char* key, const std::string& value) { cJSON_AddStringToObject(o, key, value.c_str()); }

bool get_bool(const cJSON* o, const char* key, bool& out) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(o, key);
    if (!cJSON_IsBool(item)) return false;
    out = cJSON_IsTrue(item);
    return true;
}

bool get_num(const cJSON* o, const char* key, double& out) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(o, key);
    if (!cJSON_IsNumber(item)) return false;
    out = item->valuedouble;
    return true;
}

bool get_str(const cJSON* o, const char* key, std::string& out) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(o, key);
    if (!cJSON_IsString(item) || !item->valuestring) return false;
    out = item->valuestring;
    return true;
}

uint8_t bcd_to_bin(uint8_t v) { return static_cast<uint8_t>((v >> 4) * 10 + (v & 0x0F)); }
uint8_t bin_to_bcd(uint8_t v) { return static_cast<uint8_t>(((v / 10) << 4) | (v % 10)); }

int64_t day_key_for(int64_t epoch) noexcept {
    if (epoch <= 0) return 0;
    std::tm tm{};
    time_t t = static_cast<time_t>(epoch);
    gmtime_r(&t, &tm);
    return static_cast<int64_t>(tm.tm_year + 1900) * 10000 + (tm.tm_mon + 1) * 100 + tm.tm_mday;
}

std::string json_print(cJSON* root) {
    char* raw = cJSON_PrintUnformatted(root);
    std::string result = raw ? raw : "{}";
    if (raw) cJSON_free(raw);
    return result;
}

} // namespace

AppConfig AppConfig::defaults() {
    AppConfig cfg{};
    cfg.filters[0] = {"PP Sediment", 180, 5000.0F, VolumeSource::Feed};
    cfg.filters[1] = {"Carbon GAC", 180, 5000.0F, VolumeSource::Feed};
    cfg.filters[2] = {"Carbon Block", 180, 5000.0F, VolumeSource::Feed};
    cfg.filters[3] = {"RO Membrane", 730, 15000.0F, VolumeSource::Pure};
    cfg.filters[4] = {"Postfilter", 365, 5000.0F, VolumeSource::Pure};
    return cfg;
}

esp_err_t Store::mount_spiffs() noexcept {
    esp_vfs_spiffs_conf_t conf{};
    conf.base_path = SPIFFS_BASE;
    conf.partition_label = "storage";
    conf.max_files = 8;
    conf.format_if_mount_failed = true;
    const esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err == ESP_ERR_INVALID_STATE) return ESP_OK;
    return err;
}

esp_err_t Store::init() noexcept {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "erase NVS");
        err = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(err, TAG, "init NVS");
    return mount_spiffs();
}

std::string Store::config_json(const AppConfig& cfg, bool include_secrets) const {
    cJSON* root = cJSON_CreateObject();
    add_num(root, "version", cfg.version);

    cJSON* ctl = cJSON_AddObjectToObject(root, "controller");
    add_num(ctl, "boot_stabilize_ms", cfg.controller.boot_stabilize_ms);
    add_num(ctl, "low_pressure_stable_ms", cfg.controller.low_pressure_stable_ms);
    add_num(ctl, "low_pressure_restart_delay_ms", cfg.controller.low_pressure_restart_delay_ms);
    add_num(ctl, "tank_full_debounce_ms", cfg.controller.tank_full_debounce_ms);
    add_num(ctl, "prepare_ms", cfg.controller.prepare_ms);
    add_num(ctl, "flush_duration_ms", cfg.controller.flush_duration_ms);
    add_num(ctl, "long_idle_ms", cfg.controller.long_idle_ms);
    add_num(ctl, "standby_flush_interval_s", cfg.controller.standby_flush_interval_s);
    add_num(ctl, "max_production_ms", cfg.controller.max_production_ms);
    add_num(ctl, "service_test_timeout_ms", cfg.controller.service_test_timeout_ms);
    add_bool(ctl, "standby_flush_enabled", cfg.controller.standby_flush_enabled);
    add_bool(ctl, "quiet_hours_enabled", cfg.controller.quiet_hours_enabled);
    add_num(ctl, "quiet_start_minutes", cfg.controller.quiet_start_minutes);
    add_num(ctl, "quiet_end_minutes", cfg.controller.quiet_end_minutes);

    cJSON* hw = cJSON_AddObjectToObject(root, "hardware");
    add_num(hw, "low_pressure_polarity", static_cast<int>(cfg.hardware.low_pressure_polarity));
    add_num(hw, "high_pressure_polarity", static_cast<int>(cfg.hardware.high_pressure_polarity));
    add_num(hw, "leak_polarity", static_cast<int>(cfg.hardware.leak_polarity));
    add_bool(hw, "leak_enabled", cfg.hardware.leak_enabled);
    add_bool(hw, "inlet_active_high", cfg.hardware.inlet_active_high);
    add_bool(hw, "pump_active_high", cfg.hardware.pump_active_high);
    add_bool(hw, "flush_active_high", cfg.hardware.flush_active_high);

    cJSON* wifi = cJSON_AddObjectToObject(root, "wifi");
    add_str(wifi, "ssid", cfg.wifi.ssid);
    if (include_secrets) add_str(wifi, "password", cfg.wifi.password);

    cJSON* mqtt = cJSON_AddObjectToObject(root, "mqtt");
    add_bool(mqtt, "enabled", cfg.mqtt.enabled);
    add_bool(mqtt, "tls", cfg.mqtt.tls);
    add_str(mqtt, "host", cfg.mqtt.host);
    add_num(mqtt, "port", cfg.mqtt.port);
    add_str(mqtt, "username", cfg.mqtt.username);
    if (include_secrets) add_str(mqtt, "password", cfg.mqtt.password);
    add_str(mqtt, "base_topic", cfg.mqtt.base_topic);
    add_str(mqtt, "discovery_prefix", cfg.mqtt.discovery_prefix);

    add_str(root, "timezone_name", cfg.timezone_name);
    add_str(root, "timezone_posix", cfg.timezone_posix);
    add_num(root, "fallback_utc_offset_minutes", cfg.fallback_utc_offset_minutes);

    cJSON* features = cJSON_AddObjectToObject(root, "features");
    cJSON* flow_enabled = cJSON_AddArrayToObject(features, "flow_enabled");
    cJSON* pulses = cJSON_AddArrayToObject(features, "flow_pulses_per_l");
    for (size_t i = 0; i < 3; ++i) {
        cJSON_AddItemToArray(flow_enabled, cJSON_CreateBool(cfg.features.flow_enabled[i]));
        cJSON_AddItemToArray(pulses, cJSON_CreateNumber(cfg.features.flow_pulses_per_l[i]));
    }
    add_bool(features, "tds_feed_enabled", cfg.features.tds_feed_enabled);
    add_bool(features, "tds_pure_enabled", cfg.features.tds_pure_enabled);
    add_bool(features, "recovery_enabled", cfg.features.recovery_enabled);
    add_num(features, "target_recovery_percent", cfg.features.target_recovery_percent);

    cJSON* filters = cJSON_AddArrayToObject(root, "filters");
    for (const auto& f : cfg.filters) {
        cJSON* item = cJSON_CreateObject();
        add_str(item, "name", f.name);
        add_num(item, "calendar_days", f.calendar_days);
        add_num(item, "volume_limit_l", f.volume_limit_l);
        add_num(item, "volume_source", static_cast<int>(f.volume_source));
        cJSON_AddItemToArray(filters, item);
    }

    const std::string result = json_print(root);
    cJSON_Delete(root);
    return result;
}

esp_err_t Store::apply_config_json(const std::string& json, AppConfig& cfg, bool allow_secrets) const noexcept {
    cJSON* root = cJSON_ParseWithLength(json.c_str(), json.size());
    if (!root) return ESP_ERR_INVALID_ARG;

    AppConfig next = cfg;
    double n = 0;
    bool b = false;

    if (const cJSON* ctl = cJSON_GetObjectItemCaseSensitive(root, "controller"); cJSON_IsObject(ctl)) {
        if (get_num(ctl, "boot_stabilize_ms", n)) next.controller.boot_stabilize_ms = static_cast<uint64_t>(n);
        if (get_num(ctl, "low_pressure_stable_ms", n)) next.controller.low_pressure_stable_ms = static_cast<uint64_t>(n);
        if (get_num(ctl, "low_pressure_restart_delay_ms", n)) next.controller.low_pressure_restart_delay_ms = static_cast<uint64_t>(n);
        if (get_num(ctl, "tank_full_debounce_ms", n)) next.controller.tank_full_debounce_ms = static_cast<uint64_t>(n);
        if (get_num(ctl, "prepare_ms", n)) next.controller.prepare_ms = static_cast<uint64_t>(n);
        if (get_num(ctl, "flush_duration_ms", n)) next.controller.flush_duration_ms = static_cast<uint64_t>(n);
        if (get_num(ctl, "long_idle_ms", n)) next.controller.long_idle_ms = static_cast<uint64_t>(n);
        if (get_num(ctl, "standby_flush_interval_s", n)) next.controller.standby_flush_interval_s = static_cast<uint64_t>(n);
        if (get_num(ctl, "max_production_ms", n)) next.controller.max_production_ms = static_cast<uint64_t>(n);
        if (get_num(ctl, "service_test_timeout_ms", n)) next.controller.service_test_timeout_ms = static_cast<uint64_t>(n);
        if (get_bool(ctl, "standby_flush_enabled", b)) next.controller.standby_flush_enabled = b;
        if (get_bool(ctl, "quiet_hours_enabled", b)) next.controller.quiet_hours_enabled = b;
        if (get_num(ctl, "quiet_start_minutes", n)) next.controller.quiet_start_minutes = static_cast<uint16_t>(n);
        if (get_num(ctl, "quiet_end_minutes", n)) next.controller.quiet_end_minutes = static_cast<uint16_t>(n);
    }
    if (!validate(next.controller).ok) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    if (const cJSON* hw = cJSON_GetObjectItemCaseSensitive(root, "hardware"); cJSON_IsObject(hw)) {
        if (get_num(hw, "low_pressure_polarity", n)) next.hardware.low_pressure_polarity = n != 0 ? ContactPolarity::NormallyClosed : ContactPolarity::NormallyOpen;
        if (get_num(hw, "high_pressure_polarity", n)) next.hardware.high_pressure_polarity = n != 0 ? ContactPolarity::NormallyClosed : ContactPolarity::NormallyOpen;
        if (get_num(hw, "leak_polarity", n)) next.hardware.leak_polarity = n != 0 ? ContactPolarity::NormallyClosed : ContactPolarity::NormallyOpen;
        if (get_bool(hw, "leak_enabled", b)) next.hardware.leak_enabled = b;
        if (get_bool(hw, "inlet_active_high", b)) next.hardware.inlet_active_high = b;
        if (get_bool(hw, "pump_active_high", b)) next.hardware.pump_active_high = b;
        if (get_bool(hw, "flush_active_high", b)) next.hardware.flush_active_high = b;
    }

    if (const cJSON* wifi = cJSON_GetObjectItemCaseSensitive(root, "wifi"); cJSON_IsObject(wifi)) {
        get_str(wifi, "ssid", next.wifi.ssid);
        if (allow_secrets) get_str(wifi, "password", next.wifi.password);
    }
    if (const cJSON* mqtt = cJSON_GetObjectItemCaseSensitive(root, "mqtt"); cJSON_IsObject(mqtt)) {
        if (get_bool(mqtt, "enabled", b)) next.mqtt.enabled = b;
        if (get_bool(mqtt, "tls", b)) next.mqtt.tls = b;
        get_str(mqtt, "host", next.mqtt.host);
        if (get_num(mqtt, "port", n)) next.mqtt.port = static_cast<uint16_t>(n);
        get_str(mqtt, "username", next.mqtt.username);
        if (allow_secrets) get_str(mqtt, "password", next.mqtt.password);
        get_str(mqtt, "base_topic", next.mqtt.base_topic);
        get_str(mqtt, "discovery_prefix", next.mqtt.discovery_prefix);
    }

    get_str(root, "timezone_name", next.timezone_name);
    get_str(root, "timezone_posix", next.timezone_posix);
    if (get_num(root, "fallback_utc_offset_minutes", n)) next.fallback_utc_offset_minutes = static_cast<int16_t>(n);

    if (const cJSON* features = cJSON_GetObjectItemCaseSensitive(root, "features"); cJSON_IsObject(features)) {
        if (const cJSON* arr = cJSON_GetObjectItemCaseSensitive(features, "flow_enabled"); cJSON_IsArray(arr)) {
            for (size_t i = 0; i < 3; ++i) {
                const cJSON* item = cJSON_GetArrayItem(arr, static_cast<int>(i));
                if (cJSON_IsBool(item)) next.features.flow_enabled[i] = cJSON_IsTrue(item);
            }
        }
        if (const cJSON* arr = cJSON_GetObjectItemCaseSensitive(features, "flow_pulses_per_l"); cJSON_IsArray(arr)) {
            for (size_t i = 0; i < 3; ++i) {
                const cJSON* item = cJSON_GetArrayItem(arr, static_cast<int>(i));
                if (cJSON_IsNumber(item) && item->valuedouble > 0.0) next.features.flow_pulses_per_l[i] = static_cast<float>(item->valuedouble);
            }
        }
        if (get_bool(features, "tds_feed_enabled", b)) next.features.tds_feed_enabled = b;
        if (get_bool(features, "tds_pure_enabled", b)) next.features.tds_pure_enabled = b;
        if (get_bool(features, "recovery_enabled", b)) next.features.recovery_enabled = b;
        if (get_num(features, "target_recovery_percent", n) && n >= 5.0 && n <= 60.0) next.features.target_recovery_percent = static_cast<float>(n);
    }

    if (const cJSON* filters = cJSON_GetObjectItemCaseSensitive(root, "filters"); cJSON_IsArray(filters)) {
        for (size_t i = 0; i < next.filters.size(); ++i) {
            const cJSON* item = cJSON_GetArrayItem(filters, static_cast<int>(i));
            if (!cJSON_IsObject(item)) continue;
            get_str(item, "name", next.filters[i].name);
            if (get_num(item, "calendar_days", n) && n >= 1 && n <= 3650) next.filters[i].calendar_days = static_cast<uint32_t>(n);
            if (get_num(item, "volume_limit_l", n) && n >= 0) next.filters[i].volume_limit_l = static_cast<float>(n);
            if (get_num(item, "volume_source", n) && n >= 0 && n <= 2) next.filters[i].volume_source = static_cast<VolumeSource>(static_cast<int>(n));
        }
    }

    cfg = std::move(next);
    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t Store::load(AppConfig& cfg, PersistentFacts& facts, std::array<FilterState,5>& filters) noexcept {
    cfg = AppConfig::defaults();
    facts = {};
    filters = {};
    nvs_handle_t h{};
    esp_err_t err = open_nvs(NVS_READONLY, h);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    ESP_RETURN_ON_ERROR(err, TAG, "open NVS");

    std::string cfg_json;
    if (load_string(h, "config", cfg_json) == ESP_OK) {
        if (apply_config_json(cfg_json, cfg, true) != ESP_OK) ESP_LOGW(TAG, "stored config invalid; defaults used");
    }
    size_t facts_size = sizeof(facts);
    if (nvs_get_blob(h, "facts", &facts, &facts_size) != ESP_OK || facts_size != sizeof(facts)) facts = {};
    for (size_t i = 0; i < filters.size(); ++i) {
        char key[12];
        std::snprintf(key, sizeof(key), "filter%u", static_cast<unsigned>(i));
        size_t size = sizeof(FilterState);
        if (nvs_get_blob(h, key, &filters[i], &size) != ESP_OK || size != sizeof(FilterState)) filters[i] = {};
    }
    nvs_close(h);
    return ESP_OK;
}

esp_err_t Store::save_config(const AppConfig& cfg) noexcept {
    nvs_handle_t h{};
    ESP_RETURN_ON_ERROR(open_nvs(NVS_READWRITE, h), TAG, "open config NVS");
    const std::string json = config_json(cfg, true);
    esp_err_t err = save_string(h, "config", json);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t Store::save_facts(const PersistentFacts& facts) noexcept {
    nvs_handle_t h{};
    ESP_RETURN_ON_ERROR(open_nvs(NVS_READWRITE, h), TAG, "open facts NVS");
    esp_err_t err = nvs_set_blob(h, "facts", &facts, sizeof(facts));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t Store::save_filter_state(size_t index, const FilterState& state) noexcept {
    if (index >= 5) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h{};
    ESP_RETURN_ON_ERROR(open_nvs(NVS_READWRITE, h), TAG, "open filter NVS");
    char key[12];
    std::snprintf(key, sizeof(key), "filter%u", static_cast<unsigned>(index));
    esp_err_t err = nvs_set_blob(h, key, &state, sizeof(state));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t Store::load_tls_identity(std::string& cert_pem, std::string& key_pem) noexcept {
    nvs_handle_t h{};
    ESP_RETURN_ON_ERROR(open_nvs(NVS_READONLY, h), TAG, "open TLS NVS");
    esp_err_t err = load_string(h, "tls_cert", cert_pem);
    if (err == ESP_OK) err = load_string(h, "tls_key", key_pem);
    nvs_close(h);
    return err;
}

esp_err_t Store::save_tls_identity(const std::string& cert_pem, const std::string& key_pem) noexcept {
    nvs_handle_t h{};
    ESP_RETURN_ON_ERROR(open_nvs(NVS_READWRITE, h), TAG, "open TLS NVS");
    esp_err_t err = save_string(h, "tls_cert", cert_pem);
    if (err == ESP_OK) err = save_string(h, "tls_key", key_pem);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t Store::clear_admin() noexcept {
    nvs_handle_t h{};
    ESP_RETURN_ON_ERROR(open_nvs(NVS_READWRITE, h), TAG, "open admin NVS");
    std::string cfg_json;
    AppConfig cfg = AppConfig::defaults();
    if (load_string(h, "config", cfg_json) == ESP_OK) apply_config_json(cfg_json, cfg, true);
    cfg.admin = {};
    esp_err_t err = save_string(h, "config", config_json(cfg, true));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t Store::factory_reset() noexcept {
    nvs_handle_t h{};
    ESP_RETURN_ON_ERROR(open_nvs(NVS_READWRITE, h), TAG, "open NVS reset");
    esp_err_t err = nvs_erase_all(h);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) return err;
    std::remove(EVENT_PATH);
    std::remove(STATS_PATH);
    return ESP_OK;
}

bool Security::set_admin_password(AppConfig& cfg, const std::string& password) noexcept {
    if (password.size() < 8 || password.size() > 128) return false;
    esp_fill_random(cfg.admin.salt.data(), cfg.admin.salt.size());
    if (!pbkdf2_sha256(password, cfg.admin.salt.data(), cfg.admin.salt.size(),
                       PASSWORD_PBKDF2_ROUNDS, cfg.admin.hash.data(), cfg.admin.hash.size())) return false;
    cfg.admin.configured = true;
    return true;
}

bool Security::verify_admin_password(const AppConfig& cfg, const std::string& password) noexcept {
    if (!cfg.admin.configured) return false;
    std::array<uint8_t,32> hash{};
    if (!pbkdf2_sha256(password, cfg.admin.salt.data(), cfg.admin.salt.size(),
                       PASSWORD_PBKDF2_ROUNDS, hash.data(), hash.size())) return false;
    return ct_equal(hash.data(), cfg.admin.hash.data(), hash.size());
}

std::string Security::random_hex(size_t bytes) {
    static constexpr char HEX[] = "0123456789abcdef";
    std::vector<uint8_t> data(bytes);
    esp_fill_random(data.data(), data.size());
    std::string out(bytes * 2, '0');
    for (size_t i = 0; i < bytes; ++i) {
        out[2*i] = HEX[data[i] >> 4];
        out[2*i+1] = HEX[data[i] & 0x0F];
    }
    return out;
}

bool Security::generate_tls_identity(std::string& cert_pem, std::string& key_pem) noexcept {
    mbedtls_pk_context key;
    mbedtls_pk_init(&key);
    mbedtls_x509write_cert crt;
    mbedtls_x509write_crt_init(&crt);
    mbedtls_mpi serial;
    mbedtls_mpi_init(&serial);

    bool ok = false;
    do {
        if (mbedtls_pk_setup(&key, mbedtls_pk_info_from_type(MBEDTLS_PK_RSA)) != 0) break;
        if (mbedtls_rsa_gen_key(mbedtls_pk_rsa(key), esp_rng, nullptr, 2048, 65537) != 0) break;
        if (mbedtls_mpi_lset(&serial, 1) != 0) break;

        mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
        mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
        mbedtls_x509write_crt_set_subject_key(&crt, &key);
        mbedtls_x509write_crt_set_issuer_key(&crt, &key);
        if (mbedtls_x509write_crt_set_serial(&crt, &serial) != 0) break;
        if (mbedtls_x509write_crt_set_subject_name(&crt, "CN=ro-controller.local,O=RO Controller") != 0) break;
        if (mbedtls_x509write_crt_set_issuer_name(&crt, "CN=ro-controller.local,O=RO Controller") != 0) break;
        if (mbedtls_x509write_crt_set_validity(&crt, "20260101000000", "20451231235959") != 0) break;
        if (mbedtls_x509write_crt_set_basic_constraints(&crt, 1, -1) != 0) break;
        if (mbedtls_x509write_crt_set_subject_key_identifier(&crt) != 0) break;
        if (mbedtls_x509write_crt_set_authority_key_identifier(&crt) != 0) break;

        std::array<unsigned char, 4096> cert{};
        std::array<unsigned char, 4096> priv{};
        if (mbedtls_x509write_crt_pem(&crt, cert.data(), cert.size(), esp_rng, nullptr) != 0) break;
        if (mbedtls_pk_write_key_pem(&key, priv.data(), priv.size()) != 0) break;
        cert_pem.assign(reinterpret_cast<const char*>(cert.data()));
        key_pem.assign(reinterpret_cast<const char*>(priv.data()));
        ok = true;
    } while (false);

    mbedtls_mpi_free(&serial);
    mbedtls_x509write_crt_free(&crt);
    mbedtls_pk_free(&key);
    return ok;
}

std::string Security::encrypt_backup(const std::string& plaintext, const std::string& passphrase) {
    if (passphrase.size() < 8) return {};
    std::array<uint8_t,16> salt{};
    std::array<uint8_t,12> iv{};
    std::array<uint8_t,32> key{};
    std::array<uint8_t,16> tag{};
    esp_fill_random(salt.data(), salt.size());
    esp_fill_random(iv.data(), iv.size());
    if (!pbkdf2_sha256(passphrase, salt.data(), salt.size(), BACKUP_PBKDF2_ROUNDS, key.data(), key.size())) return {};

    std::vector<uint8_t> cipher(plaintext.size());
    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    if (mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key.data(), 256) != 0) {
        mbedtls_gcm_free(&gcm);
        return {};
    }
    const char aad[] = "ROBK1";
    const int rc = mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT,
        plaintext.size(), iv.data(), iv.size(), reinterpret_cast<const uint8_t*>(aad), sizeof(aad)-1,
        reinterpret_cast<const uint8_t*>(plaintext.data()), cipher.data(), tag.size(), tag.data());
    mbedtls_gcm_free(&gcm);
    if (rc != 0) return {};

    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "format", "ROBK1");
    cJSON_AddNumberToObject(root, "rounds", BACKUP_PBKDF2_ROUNDS);
    cJSON_AddStringToObject(root, "salt", b64_encode(salt.data(), salt.size()).c_str());
    cJSON_AddStringToObject(root, "iv", b64_encode(iv.data(), iv.size()).c_str());
    cJSON_AddStringToObject(root, "tag", b64_encode(tag.data(), tag.size()).c_str());
    cJSON_AddStringToObject(root, "data", b64_encode(cipher.data(), cipher.size()).c_str());
    std::string out = json_print(root);
    cJSON_Delete(root);
    return out;
}

std::optional<std::string> Security::decrypt_backup(const std::string& envelope, const std::string& passphrase) {
    cJSON* root = cJSON_ParseWithLength(envelope.c_str(), envelope.size());
    if (!root) return std::nullopt;
    std::string format, salt_s, iv_s, tag_s, data_s;
    double rounds_d = 0;
    const bool parsed = get_str(root, "format", format) && format == "ROBK1" &&
        get_num(root, "rounds", rounds_d) && get_str(root, "salt", salt_s) &&
        get_str(root, "iv", iv_s) && get_str(root, "tag", tag_s) && get_str(root, "data", data_s);
    cJSON_Delete(root);
    if (!parsed || rounds_d < 10'000 || rounds_d > 1'000'000) return std::nullopt;
    auto salt = b64_decode(salt_s); auto iv = b64_decode(iv_s); auto tag = b64_decode(tag_s); auto data = b64_decode(data_s);
    if (!salt || !iv || !tag || !data || salt->size() != 16 || iv->size() != 12 || tag->size() != 16) return std::nullopt;

    std::array<uint8_t,32> key{};
    if (!pbkdf2_sha256(passphrase, salt->data(), salt->size(), static_cast<uint32_t>(rounds_d), key.data(), key.size())) return std::nullopt;
    std::vector<uint8_t> plain(data->size());
    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    if (mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key.data(), 256) != 0) {
        mbedtls_gcm_free(&gcm);
        return std::nullopt;
    }
    const char aad[] = "ROBK1";
    const int rc = mbedtls_gcm_auth_decrypt(&gcm, data->size(), iv->data(), iv->size(),
        reinterpret_cast<const uint8_t*>(aad), sizeof(aad)-1, tag->data(), tag->size(),
        data->data(), plain.data());
    mbedtls_gcm_free(&gcm);
    if (rc != 0) return std::nullopt;
    return std::string(reinterpret_cast<const char*>(plain.data()), plain.size());
}

TimeService* TimeService::instance_ = nullptr;

esp_err_t TimeService::init(i2c_master_bus_handle_t bus, const AppConfig& cfg) noexcept {
    apply_timezone(cfg);
    instance_ = this;
    if (bus) {
        i2c_device_config_t dev{};
        dev.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        dev.device_address = 0x68;
        dev.scl_speed_hz = 100000;
        if (i2c_master_bus_add_device(bus, &dev, &rtc_dev_) == ESP_OK) {
            int64_t epoch = 0;
            if (read_rtc(epoch) == ESP_OK && epoch > 1'700'000'000) {
                timeval tv{static_cast<time_t>(epoch), 0};
                settimeofday(&tv, nullptr);
                rtc_time_loaded_ = true;
            } else {
                i2c_master_bus_rm_device(rtc_dev_);
                rtc_dev_ = nullptr;
            }
        }
    }
    return ESP_OK;
}

void TimeService::apply_timezone(const AppConfig& cfg) noexcept {
    timezone_posix_ = cfg.timezone_posix.empty() ? "UTC0" : cfg.timezone_posix;
    setenv("TZ", timezone_posix_.c_str(), 1);
    tzset();
}

void TimeService::start_sntp() noexcept {
    if (sntp_started_) return;
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_setservername(1, "time.cloudflare.com");
    esp_sntp_set_time_sync_notification_cb(&TimeService::sntp_sync_cb);
    esp_sntp_init();
    sntp_started_ = true;
}

void TimeService::sntp_sync_cb(struct timeval* tv) {
    if (!instance_ || !tv) return;
    instance_->ntp_synced_ = true;
    if (instance_->rtc_dev_) instance_->write_rtc(tv->tv_sec);
}

TimeInfo TimeService::now() noexcept {
    timeval tv{};
    gettimeofday(&tv, nullptr);
    const bool valid = tv.tv_sec > 1'700'000'000;
    TimeInfo info{};
    info.valid = valid;
    info.utc_epoch_s = valid ? tv.tv_sec : 0;
    if (valid) {
        time_t t = tv.tv_sec;
        std::tm local{};
        localtime_r(&t, &local);
        info.local_minute = static_cast<uint16_t>(local.tm_hour * 60 + local.tm_min);
    }
    return info;
}

const char* TimeService::source_name() const noexcept {
    if (ntp_synced_) return "NTP";
    if (rtc_time_loaded_) return "RTC";
    return "UNKNOWN";
}

std::string TimeService::posix_for_browser_timezone(const std::string& iana, int offset_minutes) {
    if (iana == "Europe/Kyiv" || iana == "Europe/Kiev") return "EET-2EEST,M3.5.0/3,M10.5.0/4";
    if (iana == "Europe/Warsaw") return "CET-1CEST,M3.5.0/2,M10.5.0/3";
    if (iana == "Europe/London") return "GMT0BST,M3.5.0/1,M10.5.0/2";
    if (iana == "America/New_York") return "EST5EDT,M3.2.0/2,M11.1.0/2";
    const int abs_min = std::abs(offset_minutes);
    const int h = abs_min / 60;
    const int m = abs_min % 60;
    char buf[32];
    // POSIX TZ signs are reversed relative to UTC offsets.
    std::snprintf(buf, sizeof(buf), "UTC%c%d:%02d", offset_minutes >= 0 ? '-' : '+', h, m);
    return buf;
}

esp_err_t TimeService::read_rtc(int64_t& epoch) noexcept {
    if (!rtc_dev_) return ESP_ERR_INVALID_STATE;
    uint8_t reg = 0;
    uint8_t d[7]{};
    ESP_RETURN_ON_ERROR(i2c_master_transmit_receive(rtc_dev_, &reg, 1, d, sizeof(d), 100), TAG, "read RTC");
    std::tm tm{};
    tm.tm_sec = bcd_to_bin(d[0] & 0x7F);
    tm.tm_min = bcd_to_bin(d[1] & 0x7F);
    tm.tm_hour = bcd_to_bin(d[2] & 0x3F);
    tm.tm_mday = bcd_to_bin(d[4] & 0x3F);
    tm.tm_mon = bcd_to_bin(d[5] & 0x1F) - 1;
    tm.tm_year = bcd_to_bin(d[6]) + 100;
    if (tm.tm_year < 124 || tm.tm_mon < 0 || tm.tm_mon > 11 || tm.tm_mday < 1 || tm.tm_mday > 31) return ESP_ERR_INVALID_RESPONSE;
    epoch = static_cast<int64_t>(timegm(&tm));
    return epoch > 0 ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

esp_err_t TimeService::write_rtc(int64_t epoch) noexcept {
    if (!rtc_dev_ || epoch <= 0) return ESP_ERR_INVALID_STATE;
    time_t t = static_cast<time_t>(epoch);
    std::tm tm{};
    gmtime_r(&t, &tm);
    uint8_t d[8]{};
    d[0] = 0;
    d[1] = bin_to_bcd(tm.tm_sec);
    d[2] = bin_to_bcd(tm.tm_min);
    d[3] = bin_to_bcd(tm.tm_hour);
    d[4] = bin_to_bcd(static_cast<uint8_t>(tm.tm_wday == 0 ? 7 : tm.tm_wday));
    d[5] = bin_to_bcd(tm.tm_mday);
    d[6] = bin_to_bcd(static_cast<uint8_t>(tm.tm_mon + 1));
    d[7] = bin_to_bcd(static_cast<uint8_t>((tm.tm_year + 1900) - 2000));
    return i2c_master_transmit(rtc_dev_, d, sizeof(d), 100);
}

esp_err_t EventLog::append(int64_t epoch, const std::string& event) noexcept {
    std::ofstream f(EVENT_PATH, std::ios::app);
    if (!f) return ESP_FAIL;
    f << epoch << '\t' << event << '\n';
    f.close();
    compact_if_needed();
    return ESP_OK;
}

std::string EventLog::tail(size_t max_lines) const {
    std::ifstream f(EVENT_PATH);
    if (!f) return {};
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(f, line)) {
        lines.push_back(line);
        if (lines.size() > max_lines) lines.erase(lines.begin());
    }
    std::ostringstream out;
    for (const auto& l : lines) out << l << '\n';
    return out.str();
}

esp_err_t EventLog::clear() noexcept { return std::remove(EVENT_PATH) == 0 || errno == ENOENT ? ESP_OK : ESP_FAIL; }

void EventLog::compact_if_needed() noexcept {
    struct stat st{};
    if (stat(EVENT_PATH, &st) != 0 || static_cast<size_t>(st.st_size) <= EVENT_MAX_BYTES) return;
    std::ifstream in(EVENT_PATH, std::ios::binary);
    if (!in) return;
    const auto start = std::max<std::streamoff>(0, static_cast<std::streamoff>(st.st_size) - static_cast<std::streamoff>(EVENT_KEEP_BYTES));
    in.seekg(start);
    std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const auto nl = data.find('\n');
    if (nl != std::string::npos) data.erase(0, nl + 1);
    std::ofstream out(EVENT_PATH, std::ios::binary | std::ios::trunc);
    if (out) out.write(data.data(), static_cast<std::streamsize>(data.size()));
}

esp_err_t StatisticsService::init() noexcept { return ESP_OK; }

void StatisticsService::rollover_if_needed(int64_t epoch) noexcept {
    const int64_t key = day_key_for(epoch);
    if (key == 0) return;
    if (current_.day_key == 0) current_.day_key = key;
    if (key != current_.day_key) {
        append_current();
        current_ = {};
        current_.day_key = key;
    }
}

void StatisticsService::observe_second(const SystemSnapshot& snapshot, int64_t epoch) noexcept {
    if (epoch <= 0 || epoch == last_observed_epoch_) return;
    rollover_if_needed(epoch);
    if (snapshot.outputs.pump) ++current_.pump_runtime_s;
    last_observed_epoch_ = epoch;
}

void StatisticsService::on_transition(State from, State to, int64_t epoch) noexcept {
    rollover_if_needed(epoch);
    if (to == State::Producing && from != State::StartupFlush) ++current_.production_cycles;
    if (to == State::FinalFlush) ++current_.final_flushes;
    if (to == State::StandbyFlush && from == State::Standby) ++current_.standby_flushes;
}

std::string StatisticsService::current_json() const {
    cJSON* root = cJSON_CreateObject();
    add_num(root, "day", current_.day_key);
    add_num(root, "pump_runtime_s", static_cast<double>(current_.pump_runtime_s));
    add_num(root, "production_cycles", current_.production_cycles);
    add_num(root, "final_flushes", current_.final_flushes);
    add_num(root, "standby_flushes", current_.standby_flushes);
    add_num(root, "manual_flushes", current_.manual_flushes);
    add_num(root, "feed_l", current_.feed_l);
    add_num(root, "pure_l", current_.pure_l);
    add_num(root, "drain_l", current_.drain_l);
    std::string out = json_print(root);
    cJSON_Delete(root);
    return out;
}

std::string StatisticsService::history_tail(size_t max_lines) const {
    std::ifstream f(STATS_PATH);
    if (!f) return {};
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(f, line)) {
        lines.push_back(line);
        if (lines.size() > max_lines) lines.erase(lines.begin());
    }
    std::ostringstream out;
    for (const auto& l : lines) out << l << '\n';
    return out.str();
}

esp_err_t StatisticsService::append_current() noexcept {
    if (current_.day_key == 0) return ESP_OK;
    std::ofstream f(STATS_PATH, std::ios::app);
    if (!f) return ESP_FAIL;
    f << current_json() << '\n';
    return ESP_OK;
}

esp_err_t StatisticsService::checkpoint() noexcept {
    // Daily history is append-only on rollover. Runtime state is intentionally RAM-only;
    // long-term water/filter totals are checkpointed separately by FilterService/flow logic.
    return ESP_OK;
}

esp_err_t FilterService::reset(size_t index, int64_t now_epoch) noexcept {
    if (index >= states_.size()) return ESP_ERR_INVALID_ARG;
    states_[index].installation_utc_s = now_epoch;
    states_[index].consumed_ml = 0;
    return store_.save_filter_state(index, states_[index]);
}

void FilterService::add_volume(VolumeSource source, uint64_t ml) noexcept {
    for (size_t i = 0; i < states_.size(); ++i) {
        if (cfg_.filters[i].volume_source == source) states_[i].consumed_ml += ml;
    }
}

std::string FilterService::status_json(int64_t now_epoch, bool time_valid) const {
    cJSON* root = cJSON_CreateArray();
    for (size_t i = 0; i < states_.size(); ++i) {
        const auto& c = cfg_.filters[i];
        const auto& s = states_[i];
        cJSON* item = cJSON_CreateObject();
        add_num(item, "index", i);
        add_str(item, "name", c.name);
        add_num(item, "installation_utc_s", static_cast<double>(s.installation_utc_s));
        add_num(item, "consumed_l", static_cast<double>(s.consumed_ml) / 1000.0);
        add_num(item, "calendar_days", c.calendar_days);
        add_num(item, "volume_limit_l", c.volume_limit_l);
        const bool volume_expired = c.volume_limit_l > 0 && static_cast<double>(s.consumed_ml) >= static_cast<double>(c.volume_limit_l) * 1000.0;
        bool calendar_expired = false;
        if (time_valid && s.installation_utc_s > 0) {
            calendar_expired = now_epoch - s.installation_utc_s >= static_cast<int64_t>(c.calendar_days) * 86400;
        }
        add_bool(item, "expired", volume_expired || calendar_expired);
        add_bool(item, "volume_expired", volume_expired);
        add_bool(item, "calendar_expired", calendar_expired);
        cJSON_AddItemToArray(root, item);
    }
    std::string out = json_print(root);
    cJSON_Delete(root);
    return out;
}

} // namespace ro::svc
