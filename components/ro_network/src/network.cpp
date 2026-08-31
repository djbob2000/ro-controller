#include "ro/network.hpp"

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_https_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mdns.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace ro::net {
namespace {
constexpr char TAG[] = "ro_network";
constexpr uint64_t SESSION_TTL_MS = 24ULL * 60 * 60 * 1000;
constexpr uint64_t MQTT_PUBLISH_PERIOD_MS = 5000;

const char INDEX_HTML[] = R"HTML(<!doctype html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>RO Controller</title><style>
body{font-family:system-ui,sans-serif;background:#0b1118;color:#e8eef5;margin:0;padding:20px}main{max-width:760px;margin:auto}
.card{background:#141d27;border:1px solid #283747;border-radius:12px;padding:16px;margin:12px 0}.grid{display:grid;grid-template-columns:1fr 1fr;gap:8px}
button,input{font:inherit;padding:10px;border-radius:8px;border:1px solid #3b4c5e;background:#0f1720;color:#e8eef5}button{cursor:pointer}.ok{color:#75e6a6}.bad{color:#ff8e8e}pre{white-space:pre-wrap;word-break:break-word}label{display:block;margin:8px 0}
</style></head><body><main><h1>RO Controller</h1><div id="login" class="card"><h3>Login / first setup</h3><label>Admin password <input id="pw" type="password"></label><label>Wi-Fi SSID <input id="ssid"></label><label>Wi-Fi password <input id="wpw" type="password"></label><button onclick="login()">Login</button> <button onclick="setup()">First setup</button><div id="msg"></div></div>
<div id="app" style="display:none"><div class="card"><div class="grid"><div>State: <b id="state">--</b></div><div>Error: <b id="error">--</b></div><div>Water: <b id="water">--</b></div><div>Tank: <b id="tank">--</b></div><div>Pump: <b id="pump">--</b></div><div>Flush: <b id="flush">--</b></div><div>Wi-Fi: <b id="wifi">--</b></div><div>Time: <b id="time">--</b></div></div></div>
<div class="card"><button onclick="post('/api/actions/flush')">Manual flush</button> <button onclick="post('/api/actions/reset-error')">Reset error</button></div>
<div class="card"><h3>Filters</h3><pre id="filters">--</pre></div><div class="card"><h3>Statistics</h3><pre id="stats">--</pre></div><div class="card"><h3>Events</h3><pre id="events">--</pre></div></div></main>
<script>
let token=localStorage.roToken||''; const H=()=>token?{'Authorization':'Bearer '+token}:{};
async function login(){let r=await fetch('/api/login',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({password:pw.value})});let j=await r.json();if(r.ok){token=j.token;localStorage.roToken=token;show()}else msg.textContent=j.error||'login failed'}
async function setup(){let r=await fetch('/api/setup',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({password:pw.value,ssid:ssid.value,wifi_password:wpw.value,timezone_name:Intl.DateTimeFormat().resolvedOptions().timeZone||'Etc/UTC',utc_offset_minutes:-new Date().getTimezoneOffset()})});let j=await r.json();msg.textContent=j.message||j.error||'saved'}
async function get(p){let r=await fetch(p,{headers:H()});if(r.status===401)throw 0;return r.json()}
async function post(p){await fetch(p,{method:'POST',headers:H()});setTimeout(refresh,250)}
async function refresh(){try{let s=await get('/api/state');state.textContent=s.state;error.textContent=s.error;water.textContent=s.water_available?'OK':'LOW';tank.textContent=s.tank_full?'FULL':'FILLING';pump.textContent=s.pump?'ON':'OFF';flush.textContent=s.flush?'ON':'OFF';wifi.textContent=s.wifi_connected?'ONLINE':'OFFLINE';time.textContent=s.time_source;filters.textContent=JSON.stringify(await get('/api/filters'),null,2);stats.textContent=JSON.stringify(await get('/api/stats'),null,2);let er=await fetch('/api/events',{headers:H()});events.textContent=await er.text()}catch(e){app.style.display='none';login.style.display='block'}}
function show(){login.style.display='none';app.style.display='block';refresh();setInterval(refresh,3000)} if(token)show();
</script></body></html>)HTML";

std::string json_error(const char* message) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "error", message);
    char* raw = cJSON_PrintUnformatted(root);
    std::string out = raw ? raw : "{\"error\":\"unknown\"}";
    if (raw) cJSON_free(raw);
    cJSON_Delete(root);
    return out;
}

std::string json_message(const char* message) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "message", message);
    char* raw = cJSON_PrintUnformatted(root);
    std::string out = raw ? raw : "{}";
    if (raw) cJSON_free(raw);
    cJSON_Delete(root);
    return out;
}

std::string json_string(const std::string& value) {
    cJSON* root = cJSON_CreateString(value.c_str());
    char* raw = cJSON_PrintUnformatted(root);
    std::string out = raw ? raw : "\"\"";
    if (raw) cJSON_free(raw);
    cJSON_Delete(root);
    return out;
}

} // namespace

NetworkManager::NetworkManager(svc::Store& store, svc::AppConfig& config, svc::TimeService& time,
                               svc::EventLog& events, svc::StatisticsService& stats,
                               svc::FilterService& filters, Hooks hooks) noexcept
    : store_(store), config_(config), time_(time), events_(events), stats_(stats), filters_(filters), hooks_(hooks) {}

esp_err_t NetworkManager::init() noexcept {
    ESP_RETURN_ON_ERROR(start_wifi(), TAG, "wifi start");
    ESP_RETURN_ON_ERROR(start_web(), TAG, "web start");
    esp_err_t mdns_err = mdns_init();
    if (mdns_err == ESP_OK || mdns_err == ESP_ERR_INVALID_STATE) {
        mdns_hostname_set("ro-controller");
        mdns_instance_name_set("RO Controller");
        mdns_service_add(nullptr, "_http", "_tcp", provisioning_mode_ ? 80 : 443, nullptr, 0);
    }
    return ESP_OK;
}

esp_err_t NetworkManager::start_wifi() noexcept {
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init), TAG, "esp_wifi_init");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &NetworkManager::wifi_event, this), TAG, "wifi handler");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &NetworkManager::wifi_event, this), TAG, "ip handler");

    if (config_.wifi.ssid.empty()) {
        provisioning_mode_ = true;
        esp_netif_create_default_wifi_ap();
        wifi_config_t ap{};
        uint8_t mac[6]{};
        esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
        std::snprintf(reinterpret_cast<char*>(ap.ap.ssid), sizeof(ap.ap.ssid), "RO-Controller-%02X%02X", mac[4], mac[5]);
        ap.ap.ssid_len = 0;
        ap.ap.channel = 1;
        ap.ap.max_connection = 4;
        ap.ap.authmode = WIFI_AUTH_OPEN;
        ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_AP), TAG, "wifi mode ap");
        ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap), TAG, "wifi ap config");
        ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi ap start");
        ip_address_ = "192.168.4.1";
        return ESP_OK;
    }

    provisioning_mode_ = false;
    esp_netif_create_default_wifi_sta();
    wifi_config_t sta{};
    std::strncpy(reinterpret_cast<char*>(sta.sta.ssid), config_.wifi.ssid.c_str(), sizeof(sta.sta.ssid) - 1);
    std::strncpy(reinterpret_cast<char*>(sta.sta.password), config_.wifi.password.c_str(), sizeof(sta.sta.password) - 1);
    sta.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    sta.sta.pmf_cfg.capable = true;
    sta.sta.pmf_cfg.required = false;
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "wifi mode sta");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &sta), TAG, "wifi sta config");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi sta start");
    return esp_wifi_connect();
}

void NetworkManager::wifi_event(void* arg, esp_event_base_t base, int32_t id, void* data) {
    auto* self = static_cast<NetworkManager*>(arg);
    if (!self) return;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        self->wifi_connected_ = false;
        self->ip_address_ = "--";
        self->stop_mqtt();
        esp_wifi_connect();
        return;
    }
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        auto* event = static_cast<ip_event_got_ip_t*>(data);
        if (event) {
            char ip[16]{};
            std::snprintf(ip, sizeof(ip), IPSTR, IP2STR(&event->ip_info.ip));
            self->ip_address_ = ip;
        }
        self->wifi_connected_ = true;
        self->time_.start_sntp();
        self->start_mqtt();
    }
}

esp_err_t NetworkManager::start_web() noexcept {
    return provisioning_mode_ ? start_http_provisioning() : start_https_application();
}

static esp_err_t register_handlers(httpd_handle_t server, NetworkManager* self) {
    const httpd_uri_t handlers[] = {
        {.uri="/", .method=HTTP_GET, .handler=&NetworkManager::root_handler, .user_ctx=self},
        {.uri="/api/state", .method=HTTP_GET, .handler=&NetworkManager::state_handler, .user_ctx=self},
        {.uri="/api/login", .method=HTTP_POST, .handler=&NetworkManager::login_handler, .user_ctx=self},
        {.uri="/api/setup", .method=HTTP_POST, .handler=&NetworkManager::setup_handler, .user_ctx=self},
        {.uri="/api/settings", .method=HTTP_GET, .handler=&NetworkManager::settings_get_handler, .user_ctx=self},
        {.uri="/api/settings", .method=HTTP_PUT, .handler=&NetworkManager::settings_put_handler, .user_ctx=self},
        {.uri="/api/actions/flush", .method=HTTP_POST, .handler=&NetworkManager::action_flush_handler, .user_ctx=self},
        {.uri="/api/actions/reset-error", .method=HTTP_POST, .handler=&NetworkManager::action_reset_handler, .user_ctx=self},
        {.uri="/api/events", .method=HTTP_GET, .handler=&NetworkManager::events_handler, .user_ctx=self},
        {.uri="/api/stats", .method=HTTP_GET, .handler=&NetworkManager::stats_handler, .user_ctx=self},
        {.uri="/api/filters", .method=HTTP_GET, .handler=&NetworkManager::filters_handler, .user_ctx=self},
        {.uri="/api/filters/reset", .method=HTTP_POST, .handler=&NetworkManager::filter_reset_handler, .user_ctx=self},
        {.uri="/api/ota", .method=HTTP_POST, .handler=&NetworkManager::ota_handler, .user_ctx=self},
        {.uri="/api/backup", .method=HTTP_POST, .handler=&NetworkManager::backup_handler, .user_ctx=self},
        {.uri="/api/restore", .method=HTTP_POST, .handler=&NetworkManager::restore_handler, .user_ctx=self},
    };
    for (const auto& h : handlers) {
        esp_err_t err = httpd_register_uri_handler(server, &h);
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}

esp_err_t NetworkManager::start_http_provisioning() noexcept {
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 20;
    cfg.stack_size = 8192;
    ESP_RETURN_ON_ERROR(httpd_start(&server_, &cfg), TAG, "httpd_start");
    return register_handlers(server_, this);
}

esp_err_t NetworkManager::start_https_application() noexcept {
    if (store_.load_tls_identity(tls_cert_, tls_key_) != ESP_OK) {
        ESP_LOGI(TAG, "generating per-device TLS identity");
        if (!svc::Security::generate_tls_identity(tls_cert_, tls_key_)) return ESP_FAIL;
        ESP_RETURN_ON_ERROR(store_.save_tls_identity(tls_cert_, tls_key_), TAG, "save TLS identity");
    }

    httpd_ssl_config_t cfg = HTTPD_SSL_CONFIG_DEFAULT();
    cfg.httpd.max_uri_handlers = 20;
    cfg.httpd.stack_size = 12288;
    cfg.servercert = reinterpret_cast<const uint8_t*>(tls_cert_.c_str());
    cfg.servercert_len = tls_cert_.size() + 1;
    cfg.prvtkey_pem = reinterpret_cast<const uint8_t*>(tls_key_.c_str());
    cfg.prvtkey_len = tls_key_.size() + 1;
    ESP_RETURN_ON_ERROR(httpd_ssl_start(&server_, &cfg), TAG, "https start");
    return register_handlers(server_, this);
}

NetworkManager* NetworkManager::self(httpd_req_t* req) noexcept {
    return req ? static_cast<NetworkManager*>(req->user_ctx) : nullptr;
}

std::string NetworkManager::read_body(httpd_req_t* req, size_t max_bytes) {
    if (!req || req->content_len < 0 || static_cast<size_t>(req->content_len) > max_bytes) return {};
    std::string body(static_cast<size_t>(req->content_len), '\0');
    size_t received = 0;
    while (received < body.size()) {
        const int rc = httpd_req_recv(req, body.data() + received, body.size() - received);
        if (rc <= 0) return {};
        received += static_cast<size_t>(rc);
    }
    return body;
}

esp_err_t NetworkManager::send_json(httpd_req_t* req, const std::string& json, const char* status) {
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, json.data(), static_cast<ssize_t>(json.size()));
}

esp_err_t NetworkManager::send_error(httpd_req_t* req, const char* status, const char* message) {
    return send_json(req, json_error(message), status);
}

bool NetworkManager::session_valid() const noexcept {
    return !session_token_.empty() && static_cast<uint64_t>(esp_timer_get_time() / 1000) < session_expires_ms_;
}

bool NetworkManager::authorized(httpd_req_t* req) noexcept {
    if (!config_.admin.configured) return false;
    if (!session_valid()) return false;
    const size_t len = httpd_req_get_hdr_value_len(req, "Authorization");
    if (len == 0 || len > 160) return false;
    std::vector<char> value(len + 1);
    if (httpd_req_get_hdr_value_str(req, "Authorization", value.data(), value.size()) != ESP_OK) return false;
    const std::string expected = "Bearer " + session_token_;
    return expected == value.data();
}

std::string NetworkManager::state_json() const {
    const SystemSnapshot s = hooks_.snapshot ? hooks_.snapshot(hooks_.context) : SystemSnapshot{};
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "mode", s.mode == Mode::Auto ? "auto" : "maintenance");
    cJSON_AddStringToObject(root, "state", to_string(s.state));
    cJSON_AddStringToObject(root, "error", to_string(s.error));
    cJSON_AddBoolToObject(root, "water_available", s.inputs.water_available);
    cJSON_AddBoolToObject(root, "tank_full", s.inputs.tank_full);
    cJSON_AddBoolToObject(root, "leak_detected", s.inputs.leak_detected);
    cJSON_AddBoolToObject(root, "inlet", s.outputs.inlet);
    cJSON_AddBoolToObject(root, "pump", s.outputs.pump);
    cJSON_AddBoolToObject(root, "flush", s.outputs.flush);
    cJSON_AddNumberToObject(root, "production_runtime_s", static_cast<double>(s.production_runtime_ms) / 1000.0);
    cJSON_AddBoolToObject(root, "wifi_connected", wifi_connected_ || provisioning_mode_);
    cJSON_AddBoolToObject(root, "mqtt_connected", mqtt_connected_);
    cJSON_AddStringToObject(root, "ip", ip_address_.c_str());
    cJSON_AddStringToObject(root, "time_source", time_.source_name());
    const auto ti = time_.now();
    cJSON_AddBoolToObject(root, "time_valid", ti.valid);
    cJSON_AddNumberToObject(root, "utc_epoch_s", static_cast<double>(ti.utc_epoch_s));
    cJSON_AddNullToObject(root, "feed_flow_lpm");
    cJSON_AddNullToObject(root, "pure_flow_lpm");
    cJSON_AddNullToObject(root, "drain_flow_lpm");
    cJSON_AddNullToObject(root, "feed_tds_ppm");
    cJSON_AddNullToObject(root, "pure_tds_ppm");
    char* raw = cJSON_PrintUnformatted(root);
    std::string out = raw ? raw : "{}";
    if (raw) cJSON_free(raw);
    cJSON_Delete(root);
    return out;
}

esp_err_t NetworkManager::root_handler(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

esp_err_t NetworkManager::state_handler(httpd_req_t* req) {
    auto* n = self(req); if (!n) return ESP_FAIL;
    if (!n->authorized(req)) return send_error(req, "401 Unauthorized", "authentication required");
    return send_json(req, n->state_json());
}

esp_err_t NetworkManager::login_handler(httpd_req_t* req) {
    auto* n = self(req); if (!n) return ESP_FAIL;
    if (!n->config_.admin.configured) return send_error(req, "409 Conflict", "admin password not configured");
    const std::string body = read_body(req, 1024);
    cJSON* root = cJSON_ParseWithLength(body.c_str(), body.size());
    const cJSON* pw = root ? cJSON_GetObjectItemCaseSensitive(root, "password") : nullptr;
    if (!cJSON_IsString(pw) || !svc::Security::verify_admin_password(n->config_, pw->valuestring)) {
        if (root) cJSON_Delete(root);
        return send_error(req, "401 Unauthorized", "invalid password");
    }
    cJSON_Delete(root);
    n->session_token_ = svc::Security::random_hex(24);
    n->session_expires_ms_ = static_cast<uint64_t>(esp_timer_get_time() / 1000) + SESSION_TTL_MS;
    return send_json(req, std::string("{\"token\":") + json_string(n->session_token_) + "}");
}

esp_err_t NetworkManager::setup_handler(httpd_req_t* req) {
    auto* n = self(req); if (!n) return ESP_FAIL;
    if (n->config_.admin.configured) return send_error(req, "403 Forbidden", "setup already completed");
    const std::string body = read_body(req, 4096);
    cJSON* root = cJSON_ParseWithLength(body.c_str(), body.size());
    if (!root) return send_error(req, "400 Bad Request", "invalid JSON");
    const cJSON* pw = cJSON_GetObjectItemCaseSensitive(root, "password");
    const cJSON* ssid = cJSON_GetObjectItemCaseSensitive(root, "ssid");
    const cJSON* wifi_pw = cJSON_GetObjectItemCaseSensitive(root, "wifi_password");
    const cJSON* tz = cJSON_GetObjectItemCaseSensitive(root, "timezone_name");
    const cJSON* offset = cJSON_GetObjectItemCaseSensitive(root, "utc_offset_minutes");
    if (!cJSON_IsString(pw) || std::strlen(pw->valuestring) < 8) {
        cJSON_Delete(root);
        return send_error(req, "400 Bad Request", "password must be at least 8 characters");
    }
    if (!n->hooks_.set_admin_password || n->hooks_.set_admin_password(n->hooks_.context, pw->valuestring) != ESP_OK) {
        cJSON_Delete(root);
        return send_error(req, "500 Internal Server Error", "unable to save admin password");
    }

    cJSON* cfg = cJSON_CreateObject();
    cJSON* wifi = cJSON_AddObjectToObject(cfg, "wifi");
    cJSON_AddStringToObject(wifi, "ssid", cJSON_IsString(ssid) ? ssid->valuestring : "");
    cJSON_AddStringToObject(wifi, "password", cJSON_IsString(wifi_pw) ? wifi_pw->valuestring : "");
    if (cJSON_IsString(tz)) cJSON_AddStringToObject(cfg, "timezone_name", tz->valuestring);
    int offset_min = cJSON_IsNumber(offset) ? offset->valueint : 0;
    cJSON_AddNumberToObject(cfg, "fallback_utc_offset_minutes", offset_min);
    const std::string iana = cJSON_IsString(tz) ? tz->valuestring : "Etc/UTC";
    const std::string posix = svc::TimeService::posix_for_browser_timezone(iana, offset_min);
    cJSON_AddStringToObject(cfg, "timezone_posix", posix.c_str());
    char* raw = cJSON_PrintUnformatted(cfg);
    std::string cfg_json = raw ? raw : "{}";
    if (raw) cJSON_free(raw);
    cJSON_Delete(cfg);
    cJSON_Delete(root);

    if (!n->hooks_.apply_config || n->hooks_.apply_config(n->hooks_.context, cfg_json) != ESP_OK)
        return send_error(req, "500 Internal Server Error", "unable to save settings");
    send_json(req, json_message("saved; controller will restart"));
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

esp_err_t NetworkManager::settings_get_handler(httpd_req_t* req) {
    auto* n = self(req); if (!n) return ESP_FAIL;
    if (!n->authorized(req)) return send_error(req, "401 Unauthorized", "authentication required");
    return send_json(req, n->store_.config_json(n->config_, false));
}

esp_err_t NetworkManager::settings_put_handler(httpd_req_t* req) {
    auto* n = self(req); if (!n) return ESP_FAIL;
    if (!n->authorized(req)) return send_error(req, "401 Unauthorized", "authentication required");
    const std::string body = read_body(req);
    if (body.empty()) return send_error(req, "400 Bad Request", "empty or oversized body");
    if (!n->hooks_.apply_config || n->hooks_.apply_config(n->hooks_.context, body) != ESP_OK)
        return send_error(req, "400 Bad Request", "invalid settings");
    return send_json(req, json_message("settings saved"));
}

esp_err_t NetworkManager::action_flush_handler(httpd_req_t* req) {
    auto* n = self(req); if (!n) return ESP_FAIL;
    if (!n->authorized(req)) return send_error(req, "401 Unauthorized", "authentication required");
    if (!n->hooks_.enqueue_command || !n->hooks_.enqueue_command(n->hooks_.context, {CommandType::StartManualFlush, CommandSource::Web}))
        return send_error(req, "409 Conflict", "command rejected");
    return send_json(req, json_message("flush requested"));
}

esp_err_t NetworkManager::action_reset_handler(httpd_req_t* req) {
    auto* n = self(req); if (!n) return ESP_FAIL;
    if (!n->authorized(req)) return send_error(req, "401 Unauthorized", "authentication required");
    if (!n->hooks_.enqueue_command || !n->hooks_.enqueue_command(n->hooks_.context, {CommandType::ResetError, CommandSource::Web}))
        return send_error(req, "409 Conflict", "command rejected");
    return send_json(req, json_message("reset requested"));
}

esp_err_t NetworkManager::events_handler(httpd_req_t* req) {
    auto* n = self(req); if (!n) return ESP_FAIL;
    if (!n->authorized(req)) return send_error(req, "401 Unauthorized", "authentication required");
    const std::string data = n->events_.tail(100);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, data.data(), static_cast<ssize_t>(data.size()));
}

esp_err_t NetworkManager::stats_handler(httpd_req_t* req) {
    auto* n = self(req); if (!n) return ESP_FAIL;
    if (!n->authorized(req)) return send_error(req, "401 Unauthorized", "authentication required");
    return send_json(req, n->stats_.current_json());
}

esp_err_t NetworkManager::filters_handler(httpd_req_t* req) {
    auto* n = self(req); if (!n) return ESP_FAIL;
    if (!n->authorized(req)) return send_error(req, "401 Unauthorized", "authentication required");
    const auto t = n->time_.now();
    return send_json(req, n->filters_.status_json(t.utc_epoch_s, t.valid));
}

esp_err_t NetworkManager::filter_reset_handler(httpd_req_t* req) {
    auto* n = self(req); if (!n) return ESP_FAIL;
    if (!n->authorized(req)) return send_error(req, "401 Unauthorized", "authentication required");
    std::array<char, 64> query{};
    std::array<char, 8> index_text{};
    if (httpd_req_get_url_query_str(req, query.data(), query.size()) != ESP_OK ||
        httpd_query_key_value(query.data(), "index", index_text.data(), index_text.size()) != ESP_OK)
        return send_error(req, "400 Bad Request", "missing index");
    const int index = std::atoi(index_text.data());
    if (index < 0 || index >= 5 || !n->hooks_.reset_filter || n->hooks_.reset_filter(n->hooks_.context, static_cast<size_t>(index)) != ESP_OK)
        return send_error(req, "400 Bad Request", "invalid filter index");
    return send_json(req, json_message("filter reset"));
}

esp_err_t NetworkManager::ota_handler(httpd_req_t* req) {
    auto* n = self(req); if (!n) return ESP_FAIL;
    if (!n->authorized(req)) return send_error(req, "401 Unauthorized", "authentication required");
    if (req->content_len <= 0) return send_error(req, "400 Bad Request", "empty firmware");
    if (!n->hooks_.set_ota_hold || !n->hooks_.set_ota_hold(n->hooks_.context, true))
        return send_error(req, "409 Conflict", "unable to enter OTA hold");

    const esp_partition_t* part = esp_ota_get_next_update_partition(nullptr);
    if (!part) {
        n->hooks_.set_ota_hold(n->hooks_.context, false);
        return send_error(req, "500 Internal Server Error", "no OTA partition");
    }
    esp_ota_handle_t handle{};
    esp_err_t err = esp_ota_begin(part, static_cast<size_t>(req->content_len), &handle);
    if (err != ESP_OK) {
        n->hooks_.set_ota_hold(n->hooks_.context, false);
        return send_error(req, "500 Internal Server Error", "ota begin failed");
    }

    std::array<char, 4096> buffer{};
    int remaining = req->content_len;
    while (remaining > 0) {
        const int got = httpd_req_recv(req, buffer.data(), std::min<int>(remaining, buffer.size()));
        if (got <= 0) { err = ESP_FAIL; break; }
        err = esp_ota_write(handle, buffer.data(), static_cast<size_t>(got));
        if (err != ESP_OK) break;
        remaining -= got;
    }
    if (err == ESP_OK) err = esp_ota_end(handle);
    else esp_ota_abort(handle);
    if (err == ESP_OK) err = esp_ota_set_boot_partition(part);
    if (err != ESP_OK) {
        n->hooks_.set_ota_hold(n->hooks_.context, false);
        return send_error(req, "500 Internal Server Error", "firmware verification failed");
    }

    send_json(req, json_message("firmware accepted; rebooting"));
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

esp_err_t NetworkManager::backup_handler(httpd_req_t* req) {
    auto* n = self(req); if (!n) return ESP_FAIL;
    if (!n->authorized(req)) return send_error(req, "401 Unauthorized", "authentication required");
    const std::string body = read_body(req, 2048);
    cJSON* root = cJSON_ParseWithLength(body.c_str(), body.size());
    const cJSON* pass = root ? cJSON_GetObjectItemCaseSensitive(root, "passphrase") : nullptr;
    if (!cJSON_IsString(pass) || std::strlen(pass->valuestring) < 8) {
        if (root) cJSON_Delete(root);
        return send_error(req, "400 Bad Request", "passphrase must be at least 8 characters");
    }
    const std::string encrypted = svc::Security::encrypt_backup(n->store_.config_json(n->config_, true), pass->valuestring);
    cJSON_Delete(root);
    if (encrypted.empty()) return send_error(req, "500 Internal Server Error", "backup encryption failed");
    return send_json(req, encrypted);
}

esp_err_t NetworkManager::restore_handler(httpd_req_t* req) {
    auto* n = self(req); if (!n) return ESP_FAIL;
    if (!n->authorized(req)) return send_error(req, "401 Unauthorized", "authentication required");
    const std::string body = read_body(req, 64 * 1024);
    cJSON* root = cJSON_ParseWithLength(body.c_str(), body.size());
    const cJSON* pass = root ? cJSON_GetObjectItemCaseSensitive(root, "passphrase") : nullptr;
    const cJSON* envelope = root ? cJSON_GetObjectItemCaseSensitive(root, "envelope") : nullptr;
    if (!cJSON_IsString(pass) || !cJSON_IsObject(envelope)) {
        if (root) cJSON_Delete(root);
        return send_error(req, "400 Bad Request", "invalid restore request");
    }
    char* env_raw = cJSON_PrintUnformatted(envelope);
    const std::string env = env_raw ? env_raw : "";
    if (env_raw) cJSON_free(env_raw);
    const std::string password = pass->valuestring;
    cJSON_Delete(root);
    auto plain = svc::Security::decrypt_backup(env, password);
    if (!plain) return send_error(req, "400 Bad Request", "invalid backup or passphrase");
    if (!n->hooks_.apply_config || n->hooks_.apply_config(n->hooks_.context, *plain) != ESP_OK)
        return send_error(req, "400 Bad Request", "backup contains invalid settings");
    return send_json(req, json_message("configuration restored"));
}

void NetworkManager::start_mqtt() noexcept {
    if (!wifi_connected_ || !config_.mqtt.enabled || config_.mqtt.host.empty() || mqtt_) return;
    mqtt_uri_ = std::string(config_.mqtt.tls ? "mqtts://" : "mqtt://") + config_.mqtt.host + ":" + std::to_string(config_.mqtt.port);
    mqtt_lwt_topic_ = config_.mqtt.base_topic + "/status";
    esp_mqtt_client_config_t cfg{};
    cfg.broker.address.uri = mqtt_uri_.c_str();
    if (!config_.mqtt.username.empty()) cfg.credentials.username = config_.mqtt.username.c_str();
    if (!config_.mqtt.password.empty()) cfg.credentials.authentication.password = config_.mqtt.password.c_str();
    cfg.session.last_will.topic = mqtt_lwt_topic_.c_str();
    cfg.session.last_will.msg = "offline";
    cfg.session.last_will.msg_len = 7;
    cfg.session.last_will.qos = 1;
    cfg.session.last_will.retain = 1;
    if (config_.mqtt.tls) cfg.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
    mqtt_ = esp_mqtt_client_init(&cfg);
    if (!mqtt_) return;
    esp_mqtt_client_register_event(mqtt_, ESP_EVENT_ANY_ID, &NetworkManager::mqtt_event, this);
    if (esp_mqtt_client_start(mqtt_) != ESP_OK) {
        esp_mqtt_client_destroy(mqtt_);
        mqtt_ = nullptr;
    }
}

void NetworkManager::stop_mqtt() noexcept {
    mqtt_connected_ = false;
    if (!mqtt_) return;
    esp_mqtt_client_stop(mqtt_);
    esp_mqtt_client_destroy(mqtt_);
    mqtt_ = nullptr;
}

void NetworkManager::mqtt_event(void* handler_args, esp_event_base_t, int32_t id, void* event_data) {
    auto* n = static_cast<NetworkManager*>(handler_args);
    auto* e = static_cast<esp_mqtt_event_handle_t>(event_data);
    if (!n || !e) return;
    if (id == MQTT_EVENT_CONNECTED) {
        n->mqtt_connected_ = true;
        esp_mqtt_client_publish(n->mqtt_, n->mqtt_lwt_topic_.c_str(), "online", 0, 1, 1);
        const std::string flush_topic = n->config_.mqtt.base_topic + "/cmd/flush";
        const std::string reset_topic = n->config_.mqtt.base_topic + "/cmd/reset_error";
        esp_mqtt_client_subscribe(n->mqtt_, flush_topic.c_str(), 1);
        esp_mqtt_client_subscribe(n->mqtt_, reset_topic.c_str(), 1);
        n->publish_discovery();
        n->publish_state(true);
    } else if (id == MQTT_EVENT_DISCONNECTED) {
        n->mqtt_connected_ = false;
    } else if (id == MQTT_EVENT_DATA) {
        const std::string topic(e->topic, static_cast<size_t>(e->topic_len));
        const std::string payload(e->data, static_cast<size_t>(e->data_len));
        if (payload != "PRESS" && payload != "1" && payload != "ON") return;
        if (!n->hooks_.enqueue_command) return;
        if (topic == n->config_.mqtt.base_topic + "/cmd/flush")
            n->hooks_.enqueue_command(n->hooks_.context, {CommandType::StartManualFlush, CommandSource::Mqtt});
        else if (topic == n->config_.mqtt.base_topic + "/cmd/reset_error")
            n->hooks_.enqueue_command(n->hooks_.context, {CommandType::ResetError, CommandSource::Mqtt});
    }
}

void NetworkManager::publish_discovery() noexcept {
    if (!mqtt_connected_) return;
    const std::string prefix = config_.mqtt.discovery_prefix.empty() ? "homeassistant" : config_.mqtt.discovery_prefix;
    const std::string base = config_.mqtt.base_topic;
    const std::string state = base + "/state";
    const std::string device = "\"dev\":{\"ids\":[\"ro_controller\"],\"name\":\"RO Controller\",\"mf\":\"DIY\",\"mdl\":\"ESP32-S3 RO Controller\"}";
    auto pub = [&](const std::string& topic, const std::string& payload) {
        esp_mqtt_client_publish(mqtt_, topic.c_str(), payload.c_str(), 0, 1, 1);
    };
    pub(prefix + "/sensor/ro_controller_state/config",
        "{\"name\":\"State\",\"uniq_id\":\"ro_state\",\"stat_t\":\"" + state + "\",\"val_tpl\":\"{{ value_json.state }}\"," + device + "}");
    pub(prefix + "/binary_sensor/ro_controller_water/config",
        "{\"name\":\"Water available\",\"uniq_id\":\"ro_water\",\"stat_t\":\"" + state + "\",\"val_tpl\":\"{{ 'ON' if value_json.water_available else 'OFF' }}\"," + device + "}");
    pub(prefix + "/binary_sensor/ro_controller_tank/config",
        "{\"name\":\"Tank full\",\"uniq_id\":\"ro_tank\",\"stat_t\":\"" + state + "\",\"val_tpl\":\"{{ 'ON' if value_json.tank_full else 'OFF' }}\"," + device + "}");
    pub(prefix + "/binary_sensor/ro_controller_pump/config",
        "{\"name\":\"Pump\",\"uniq_id\":\"ro_pump\",\"stat_t\":\"" + state + "\",\"val_tpl\":\"{{ 'ON' if value_json.pump else 'OFF' }}\"," + device + "}");
    pub(prefix + "/button/ro_controller_flush/config",
        "{\"name\":\"Start flush\",\"uniq_id\":\"ro_flush_button\",\"cmd_t\":\"" + base + "/cmd/flush\",\"payload_press\":\"PRESS\"," + device + "}");
    pub(prefix + "/button/ro_controller_reset/config",
        "{\"name\":\"Reset error\",\"uniq_id\":\"ro_reset_button\",\"cmd_t\":\"" + base + "/cmd/reset_error\",\"payload_press\":\"PRESS\"," + device + "}");
}

void NetworkManager::publish_state(bool force) noexcept {
    if (!mqtt_connected_ || !mqtt_) return;
    const uint64_t now = static_cast<uint64_t>(esp_timer_get_time() / 1000);
    if (!force && now - last_publish_ms_ < MQTT_PUBLISH_PERIOD_MS) return;
    last_publish_ms_ = now;
    const std::string topic = config_.mqtt.base_topic + "/state";
    const std::string payload = state_json();
    esp_mqtt_client_publish(mqtt_, topic.c_str(), payload.c_str(), 0, 1, 1);
}

void NetworkManager::publish_periodic(uint64_t now_ms) noexcept {
    (void)now_ms;
    publish_state(false);
}

} // namespace ro::net
