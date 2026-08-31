#pragma once

// Compatibility shim for the storage service. The application originally used
// ESP-IDF's SPIFFS VFS API; Rev.1 intentionally stores logs/statistics on
// LittleFS because it is power-loss tolerant and better suited to frequent
// embedded metadata updates. Keeping this tiny adapter avoids coupling the
// service implementation to the filesystem backend.

#include "esp_littlefs.h"

#include <stddef.h>

typedef struct {
    const char* base_path;
    const char* partition_label;
    size_t max_files; // Ignored by LittleFS (it has no fixed open-file limit).
    bool format_if_mount_failed;
} esp_vfs_spiffs_conf_t;

static inline esp_err_t esp_vfs_spiffs_register(const esp_vfs_spiffs_conf_t* conf) {
    if (!conf) return ESP_ERR_INVALID_ARG;

    // Zero-initialize first so new fields added by newer LittleFS component
    // releases (for example blockdev) get their documented default value
    // instead of turning into -Werror=missing-field-initializers failures.
    esp_vfs_littlefs_conf_t littlefs_conf{};
    littlefs_conf.base_path = conf->base_path;
    littlefs_conf.partition_label = conf->partition_label;
    littlefs_conf.partition = nullptr;
    littlefs_conf.format_if_mount_failed = conf->format_if_mount_failed;
    littlefs_conf.read_only = false;
    littlefs_conf.dont_mount = false;
    littlefs_conf.grow_on_mount = true;
    return esp_vfs_littlefs_register(&littlefs_conf);
}
