#include "ro/services.hpp"

#include "nvs.h"

namespace ro::svc {
namespace {
constexpr char NVS_NS[] = "ro";
constexpr char ADMIN_KEY[] = "admin";
constexpr char CONFIG_KEY[] = "config";
}

esp_err_t Store::load_admin(AdminConfig& admin) noexcept {
    admin = {};
    nvs_handle_t h{};
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;
    size_t size = sizeof(admin);
    err = nvs_get_blob(h, ADMIN_KEY, &admin, &size);
    nvs_close(h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        admin = {};
        return ESP_OK;
    }
    if (err != ESP_OK || size != sizeof(admin)) {
        admin = {};
        return err == ESP_OK ? ESP_ERR_INVALID_SIZE : err;
    }
    return ESP_OK;
}

esp_err_t Store::save_admin(const AdminConfig& admin) noexcept {
    nvs_handle_t h{};
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    if (admin.configured) err = nvs_set_blob(h, ADMIN_KEY, &admin, sizeof(admin));
    else err = nvs_erase_key(h, ADMIN_KEY);
    if (err == ESP_ERR_NVS_NOT_FOUND && !admin.configured) err = ESP_OK;
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t Store::save_provisioned(const AppConfig& cfg) noexcept {
    nvs_handle_t h{};
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    // First-run provisioning is one NVS transaction: Wi-Fi/settings and the
    // administrator verifier become durable together or not at all.
    const std::string json = config_json(cfg, true);
    err = nvs_set_str(h, CONFIG_KEY, json.c_str());
    if (err == ESP_OK) {
        if (cfg.admin.configured) err = nvs_set_blob(h, ADMIN_KEY, &cfg.admin, sizeof(cfg.admin));
        else err = nvs_erase_key(h, ADMIN_KEY);
        if (err == ESP_ERR_NVS_NOT_FOUND && !cfg.admin.configured) err = ESP_OK;
    }
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

} // namespace ro::svc
