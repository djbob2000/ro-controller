#pragma once

#include "ro/controller.hpp"
#include "ro/services.hpp"

#include "esp_err.h"
#include "esp_http_server.h"
#include "mqtt_client.h"

#include <cstdint>
#include <string>

namespace ro::net {

struct Hooks {
    void* context{nullptr};
    SystemSnapshot (*snapshot)(void* context) noexcept{nullptr};
    bool (*enqueue_command)(void* context, const Command& command) noexcept{nullptr};
    bool (*set_ota_hold)(void* context, bool enabled) noexcept{nullptr};
    esp_err_t (*apply_config)(void* context, const std::string& json) noexcept{nullptr};
    esp_err_t (*set_admin_password)(void* context, const std::string& password) noexcept{nullptr};
    esp_err_t (*reset_filter)(void* context, size_t index) noexcept{nullptr};
};

class NetworkManager {
public:
    NetworkManager(svc::Store& store, svc::AppConfig& config, svc::TimeService& time,
                   svc::EventLog& events, svc::StatisticsService& stats,
                   svc::FilterService& filters, Hooks hooks) noexcept;

    esp_err_t init() noexcept;
    void publish_periodic(uint64_t now_ms) noexcept;
    bool wifi_connected() const noexcept { return wifi_connected_; }
    bool provisioning_mode() const noexcept { return provisioning_mode_; }
    bool mqtt_connected() const noexcept { return mqtt_connected_; }
    const char* ip_address() const noexcept { return ip_address_.c_str(); }

    // C callbacks registered with ESP-IDF's HTTP server. Public only because the
    // C API requires plain function pointers; they delegate immediately to user_ctx.
    static esp_err_t root_handler(httpd_req_t* req);
    static esp_err_t state_handler(httpd_req_t* req);
    static esp_err_t login_handler(httpd_req_t* req);
    static esp_err_t setup_handler(httpd_req_t* req);
    static esp_err_t settings_get_handler(httpd_req_t* req);
    static esp_err_t settings_put_handler(httpd_req_t* req);
    static esp_err_t action_flush_handler(httpd_req_t* req);
    static esp_err_t action_reset_handler(httpd_req_t* req);
    static esp_err_t events_handler(httpd_req_t* req);
    static esp_err_t stats_handler(httpd_req_t* req);
    static esp_err_t filters_handler(httpd_req_t* req);
    static esp_err_t filter_reset_handler(httpd_req_t* req);
    static esp_err_t ota_handler(httpd_req_t* req);
    static esp_err_t backup_handler(httpd_req_t* req);
    static esp_err_t restore_handler(httpd_req_t* req);

private:
    svc::Store& store_;
    svc::AppConfig& config_;
    svc::TimeService& time_;
    svc::EventLog& events_;
    svc::StatisticsService& stats_;
    svc::FilterService& filters_;
    Hooks hooks_{};

    httpd_handle_t server_{nullptr};
    esp_mqtt_client_handle_t mqtt_{nullptr};
    bool wifi_connected_{false};
    bool provisioning_mode_{false};
    bool mqtt_connected_{false};
    std::string ip_address_{"--"};
    std::string tls_cert_;
    std::string tls_key_;
    std::string session_token_;
    std::string csrf_token_;
    uint64_t session_expires_ms_{0};
    uint64_t last_publish_ms_{0};
    uint64_t last_heartbeat_ms_{0};
    std::string last_state_payload_;
    std::string device_id_{"unknown"};
    std::string mqtt_uri_;
    std::string mqtt_root_;
    std::string mqtt_lwt_topic_;

    esp_err_t start_wifi() noexcept;
    esp_err_t start_web() noexcept;
    esp_err_t start_http_provisioning() noexcept;
    esp_err_t start_https_application() noexcept;
    void start_mqtt() noexcept;
    void stop_mqtt() noexcept;
    void publish_discovery() noexcept;
    void publish_state(bool force = false) noexcept;
    std::string state_json() const;
    bool authorized(httpd_req_t* req) noexcept;
    bool write_authorized(httpd_req_t* req) noexcept;
    bool origin_allowed(httpd_req_t* req) const noexcept;
    bool session_valid() const noexcept;

    static void wifi_event(void* arg, esp_event_base_t base, int32_t id, void* data);
    static void mqtt_event(void* handler_args, esp_event_base_t base, int32_t id, void* event_data);

    static NetworkManager* self(httpd_req_t* req) noexcept;
    static std::string read_body(httpd_req_t* req, size_t max_bytes = 16 * 1024);
    static esp_err_t send_json(httpd_req_t* req, const std::string& json, const char* status = "200 OK");
    static esp_err_t send_error(httpd_req_t* req, const char* status, const char* message);
};

} // namespace ro::net
